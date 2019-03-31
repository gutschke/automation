const SerialPort = require('serialport');

/**
 * The Enttec object provides methods for communicating with an Enttec DMX
 * widget.
 */
class Enttec {
  /** Constructor for the Enttec object.
   * @param {string} port - filename of the serial port (e.g. /dev/ttyUSB0)
   */
  constructor(port) {
    this.port = port;
    this.serial = null;
    this.waitingForOpen = [ ];
    this.values = Array.from({length: this.minNumberOfSlots}, () => 0);
    this.nominalValues = [ ];
    this.fader = [ ];
    this.nextTimeOut = Date.now();
    this.refresh = () => {
      const now = Date.now();
      if (this.refresh !== null) {
        // DMX likes to receive all dimmer values at least once per second.
        // According to the Wikipedia page, that also ensures that several
        // other timing parameters stay within the expected range.
        var tm = 1000;
        // Keep track of how much we still need to fade changing dimmer values.
        if (this.fader.length > 0) {
          tm = this.fadeTimeStep;
          for (const f of this.fader) {
            this.values[f.id] =
              Math.clamp(Math.round(f.dest - f.delta*--f.steps), 0, 255);
          }
        }
        this.fader = this.fader.filter(x => x.steps > 0);
        this.sendDMXPackage(this.values).then(() => {
          this.nextTimeOut = now + tm - Math.max(0, now - this.nextTimeOut);
          clearTimeout(this.timer);
          this.timer = setTimeout(this.refresh, this.nextTimeOut - now);
        });
      }
    }
    this.refresh();
  }

  /**
   * Destructor.
   */
  destroy() {
    this.port = null;
    this.refresh = null;
    try {
      const s = this.serial;
      this.serial = null;
      s.close();
    } catch (err) {
    }
    const waitingForOpen = this.waitingForOpen;
    this.waitingForOpen = null;
    const err = new Error('Object destroyed');
    for (const w of waitingForOpen) {
      try {
        w.reject(err);
      } catch (err) {
      }
    }
  }

  /**
   * @private
   * Opens the serial connection to the Enttec DMX widget. Returns a Promise
   * to asynchronously track progress. Also sets up all the required callbacks
   * so that they can be used from asynchronous functions. If any type of
   * error is detected, close the port. It'll be re-opened by the next I/O
   * operation. It is perfectly fine for more than one asynchronous caller to
   * invoke this method concurrently.
   */
  async openPort() {
    if (this.serial !== null) {
      return this.serial;
    }
    return new Promise((resolve, reject) => {
      // Remember all the asynchronous callers that need to be notified once
      // the port is either open, or has failed to open.
      this.waitingForOpen.push({ resolve: resolve, reject: reject });

      // If there already is a pending operation to open the port, don't
      // start another one.
      if (this.waitingForOpen.length > 1) {
        return;
      }

      // Internal helper function that closes the (partially) open connection
      // and rejects all pending promises.
      function fail(msg) {
        const error = typeof msg === 'string' ? new Error(msg) : msg;
        const waitingForOpen = this.waitingForOpen;
        this.waitingForOpen = [ ];
        const s = this.serial;
        this.serial = null;
        if (s) s.close();
        for (const w of waitingForOpen) {
          try {
            w.reject(error);
          } catch (err) {
          }
        }
      }

      // Open the serial port. DMX is sent at 250kBaud, no parity, 1 start,
      // 8 data, and 2 stop bits. It requires a break condition that lasts
      // on the order of 100µs to signal the start of the frame. There should
      // be at least 12µs between the mark and the frame. Frames start with a
      // "0" label followed by up to 512 8bit dimmer values. The minimum time
      // between marks can be ensured by always sending at least 24 dimmer
      // values in each DMX package.
      const serial = new SerialPort(this.port,
                                    { baudRate: 250000,
                                      dataBits: 8,
                                      stopBits: 2,
                                      parity: 'none' }, () => {
        // Wake up the DMX bus. Unless RTS is cleared, the output driver is
        // disabled.
        serial.set({ rts: false }, (err) => {
          // Either reject the promise, or ...
          if (err) return fail(err);
          // ... resolve the promise for all waiting callers.
          const waitingForOpen = this.waitingForOpen;
          this.waitingForOpen = [ ];
          this.serial = serial;
          for (const w of waitingForOpen) {
            try {
              w.resolve(serial);
            } catch (err) {
            }
          }
        });
      }).on('error', () => { fail('serial port error'); })
        .on('end',   () => { fail('serial port closed'); })
        .setEncoding('binary');
    });
  }

  /**
   * Send the serial break condition that DMX requires in order to mark the
   * beginning of a new DMX frame. Also, ensure that the port is open, if it
   * hadn't previously been opened. This is useful after temporary I/O errors
   * (e.g. device unplugged from USB) to retry operations.
   * @param {string} data - data that should be written to Enttec DMX widget.
   */
  async write(data) {
    await this.openPort();
    // The NodeJS SerialPort implementation cannot delay changes to the baud-
    // rate until after the port has drained. So, we have to do this in two
    // system calls.
    await this.serial.drain();
    // The minimum break time should be 92µs, and the minimum make-after-break
    // time should be 12µs, but longer times are acceptable. Timing in
    // Javascript can be fragile, so using the hardware to assist is
    // preferable. We can achieve this by slowing down the baudrate and
    // sending a NUL character. Afterwards, we still have to drain the output,
    // as the hardware can't handle baudrate changes while data is on the fly.
    // A 90kBaud transmission rate results in a 100µs break, and a 22µs
    // make-after-break. In practice, the system calls will introduce delays
    // and the make-after-break is likely longer. That's OK.
    await this.serial.update({ baudRate: 90000 });
    await this.serial.write(Buffer.from([ 0 ]));
    await this.serial.drain();
    await this.serial.update({ baudRate: 250000 });
    return this.serial.write(data);
  }

  /**
   * @private
   * Send one DMX dataset. Verify that the package looks like DMX dataset.
   * Repeat as needed, if the package is very short.
   * @param {number[]} data - bytes to be send.
   */
  async sendDMXPackage(data) {
    this.log(`sendDMXPackage(${data})`);
    if (data.length < this.minNumberOfSlots || data.length > 513) {
      throw new Error('Invalid DMX package');
    }
    // Estimate how long it'll take to transmit our package at 250kbaud with
    // an 8N2 encoding.
    const timeMs = data.length*11/250 + 0.122;
    const timeout = Date.now() + this.minTimeMs;
    // Send multiple copies of the same package in order to ensure that we
    // spend at least a couple of milliseconds before making other changes.
    for (let reps = Math.max(1, Math.round(this.minTimeMs/timeMs));
         reps > 0; --reps) {
      await this.write(Buffer.from(data));
      if (timeout - Date.now() < 0) {
        break;
      }
    }
  }

  /**
   * Uses the Enttec DMX widget to set the brightness of one or more DMX-
   * addressable dimmers.
   * @param {number} id - the DMX id of the first dimmer.
   * @param {number} fadeTime - rate of change in seconds per 100% change.
   * @param {number} [...] values - one or more dimmer values on scale 0 to 1.
   */
  setDimmer(id, fadeTime, ...values) {
    if (id <= 0 || id + values.length > 513) {
      throw new Error('Invalid DMX512 identifiers');
    }
    if (this.values.length < id + values.length) {
      this.values.concat(Array.from(
        { length: id + value.length - this.values.length }, () => 0));
    }
    var i = id;
    values = values.map(x => Math.clamp(x, 0, 1));
    for (const v of values) {
      const dest = Math.round(v*255);
      const diff = dest - (this.values[i]);
      const steps = Math.max(1, Math.round(Math.abs(diff)*fadeTime*1000/
                                           (255*this.fadeTimeStep)));
      const delta = diff/steps;
      this.fader = this.fader.filter(x => x.id != i);
      this.fader.push({ id: i, dest: dest, steps: steps, delta: delta });
      ++i;
    }
    clearTimeout(this.timer);
    this.timer = setTimeout(this.refresh, this.fadeTimeStep);
    if (this.nominalValues.length < id + values.length) {
      this.nominalValues.length = id + values.length
    }
    this.nominalValues = this.nominalValues.slice(0, id).concat(
      values).concat(this.nominalValues.slice(id + values.length));
  }

  /**
   * Sets a dimmer's brightness, respecting dim-to-glow characteristics.
   * The light fixture actually has two DMX ids; the first one dims a string
   * of cold LEDs, and the second one dims a string of warm LEDs. By adjusting
   * the mix, we can shift color temperature while dimming brightness.
   * @param {number} id - DMX id of the first dimmer to adjust.
   * @param {number} fadeTime - rate of change in seconds per 100% change.
   * @param {number} [...] values - brightness on scale 0 to 1.0.
   */
  setDimToGlow(id, fadeTime, ...values) {
    // In order to achieve a dim-to-glow effect, we vary the rate of change
    // for both the warm and the cold LEDs. For warm LEDs, we start with a
    // high rate of change close to the lower end, and taper off to a slower
    // rate of change. For the cold LEDs, the rate of change varies in the
    // opposite direction.
    // The two LED strings have different weighing factors. This allows us
    // to shift overall color temperature at the expense of maximum
    // brightness.
    // Furthermore, we adjust the overall rate of change with an empirically
    // determined gamma factor.
    // As the manufacturer doesn't publish sufficiently detailed measurements
    // for either the LEDs or the drivers, parameters have been determined
    // empirically to have a pleasing visual effect.
    const warmCold = [ id, fadeTime ];
    values = values.map(x => Math.clamp(x, 0, 1));
    for (const value of values) {
      const quadratic = (v, initialRate, finalRate, weight) =>
          ((finalRate-initialRate)/(initialRate+finalRate)*v*v +
           2*initialRate/(initialRate+finalRate)*v)*weight;
      const v = Math.pow(value, 1.2);
      warmCold.push(/* cold LEDs */quadratic(v, 0.5, 1.3, 1));
      warmCold.push(/* warm LEDs */quadratic(v, 1.3, 0.5, .6));
    }
    this.setDimmer.apply(this, warmCold);

    // For regular dimmers, we only store a single value per DMX slot. On the
    // other hand, dim-to-glow dimmers actually have two consecutive DMX
    // addresses. We store the actual values in this.values, but we store
    // the dim-to-glow percentage in the first entry of the this.nominalValues
    // tuple for this dimmer.
    this.nominalValues = this.nominalValues.slice(0, id).concat(
      values.reduce((acc, _, idx, src) => acc.concat(src[idx], 0), [])).concat(
      this.nominalValues.slice(id + 2*values.length));
  }

  /**
   * Returns the brightness of the given dimmer.
   * @returns {number} brightness in the range of 0 to 1.
   */
  getDimmerValue(id) {
    return this.nominalValues[id] || 0;
  }
}
// Minimum number of DMX slots needed to guarantee that we never send faster
// than the minimum required time in between break conditions on the serial
// line.
Enttec.prototype.minNumberOfSlots = 24;
// Restrict updates to at most one every 5ms. Our dimmers sometimes drop
// DMX packets if we send faster than that. On the other hand, the Raspberry
// Pi Zero W that this code runs on is not really fast enough to execute more
// than about one DMX transaction every 5ms anyways.
Enttec.prototype.minTimeMs = 5;
// How frequently to refresh dimmers during active fading (in milliseconds).
Enttec.prototype.fadeTimeStep = 50;

const Logger = {
  /**
   * Write a debug message to the system log, escaping common special
   * characters for better readability.
   * @param {string} msg - The message to be output.
   */
  log(msg) {
    console.log(String(msg).replace(/\r/g, '\\r').replace(/\n/g, '\\n'));
  }
};
Object.assign(Enttec.prototype, Logger);

/**
 * Proposed future Javascript extension that clamps a value in a numerical range
 */
if (typeof Math.clamp === 'undefined') {
  Math.clamp = (number, min, max) => Math.max(min, Math.min(number, max));
}

module.exports = Enttec;
