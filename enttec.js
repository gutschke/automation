const ioctl      = require('ioctl');
const SerialPort = require('serialport');
const usleep     = require('sleep').usleep;

/**
 * The Enttec object provides methods for communicating with an Enttec OpenDMX
 * widget.
 */
class Enttec {
  /** Constructor for the Enttec object.
   * @param {string} [port='/dev/ttyUSB0'] - filename of the serial port
   */
  constructor(port) {
    this.port = port || '/dev/ttyUSB0';
    this.serial = null;
    this.waitingForOpen = [ ];
    this.values = Array.from({ length: 513 }, () => 0);
    this.nominalValues = Array.from({ length: this.values.length }, () => 0);
    this.fader = [ ];
    this.nextTimeOut = Date.now();
    this.nextTickIn(0);
  }

  /**
   * Destructor.
   */
  destroy() {
    this.port = null;
    clearTimeout(this.timer);
    this.timer = null;
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
   * Actual updates of the DMX dimmers happen asynchronously on a timer.
   * This allows us to fade dimmers in and out, to refresh dimmer status
   * on a 1Hz heartbeat, and to honor timing lock-outs required by some
   * dimmers.
   */
  refresh() {
    if (!this.timer) {
      // Object was already destroyed, but somebody invoked the refresh()
      // method manually.
      return;
    }
    const now = Date.now();
    // DMX likes to receive all dimmer values at least once per second,
    // possibly a lot more frequently. According to the Wikipedia page,
    // that also ensures that several other timing parameters stay within
    // the expected range. In order to keep CPU usage within reasonable
    // limits, and in order to not block the mainthread for too long (talking
    // to the serial port has some parts that run synchronously), we find
    // a compromise and only update every couple of 100ms.
    var tm = 400;
    // Change dimmer values as they fade to their new setting. This is
    // the only place where we actually adjust dimmers.
    var maxSlot = this.minNumberOfSlots;
    if (this.fader.length > 0) {
      for (const f of this.fader) {
        maxSlot = Math.max(maxSlot, f.id);
        this.values[f.id] =
          Math.clamp(Math.round(f.dest - f.delta*--f.steps), 0, 255);
      }
      this.fader = this.fader.filter(x => x.steps > 0);
      if (this.fader.length > 0) {
        tm = this.fadeTimeStepMs;
      }
    } else {
      maxSlot = this.values.length;
    }
    const values = this.values.slice(0, maxSlot);
    this.write(Buffer.from(values)).then(
      () => {
        this.nextTimeOut =
          now + Math.max(0, tm - Math.max(0, now - this.nextTimeOut));
        this.nextTickIn(this.nextTimeOut - now); },
      () => {
        this.nextTimeOut = now + 1000;
        this.nextTickIn(1000); });
  }

  /**
   * @private
   * Schedule the next refresh to happen on a timer.
   * @param {number} ms - number of microseconds until the next timer tick.
   */
  nextTickIn(ms) {
    // In most situations, it is acceptable to schedule a new timer that
    // overrides the one that is currently pending. But this is not true
    // while this.write() runs. That could lead to multiple overlapping
    // I/O operations. Suspend any new timer creations. We can rely on another
    // timer being set when the write operation is done.
    if (this.writing) {
      return;
    }
    clearTimeout(this.timer);
    this.timer = setTimeout(() => this.refresh(), ms);
  }

  /**
   * @private
   * Opens the serial connection to the Enttec OpenDMX widget. Returns a Promise
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
      const fail = (msg) => {
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
    this.log(`sendDMXPackage(${[...data]})`);
    this.writing = true;
    try {
      if (!this.serial) await this.openPort();
      // Turning on the break condition drains the serial port. This can take
      // up to 22ms. We don't really want to stop the event loop for that long.
      // So, if there still is data that might be in flight, return to the
      // event loop and come back here at a later time.
      const drainMs = 11*(this.lastByteCount || 0)/250 -
            (this.lastWrite && (Date.now() - this.lastWrite) || 0);
      if (drainMs > 0) {
        await new Promise(r => setTimeout(r, drainMs));
      }
      // The minimum break time should be 92µs, and the minimum make-after-break
      // time should be 12µs, but longer times are acceptable. Microseconds are
      // so fast that with our embedded hardware there really is no point to
      // return to the event loop. It makes more sense to wait synchronously.
      // In fact, on the Raspberry Pi Zero W, a basic ioctl() call frequently
      // takes on the order of 500µs anyway. On this particular hardware, we
      // can safely omit the calls to usleep() and timing will still be OK.
      ioctl(this.serial.binding.fd, 0x5427 /* TIOCSBRK */);
      // usleep(92);
      ioctl(this.serial.binding.fd, 0x5428 /* TIOCCBRK */);
      // usleep(12);
      return new Promise((resolve, reject) => {
        this.lastByteCount = data.length;
        this.serial.write(data, (err) => {
          this.writing = false;
          if (err) reject(err);
          else resolve(); });
        this.lastWrite = Date.now(); });
    } catch (err) {
      this.writing = false;
      throw err;
    }
  }

  /**
   * Uses the Enttec OpenDMX widget to set the brightness of one or more DMX-
   * addressable dimmers.
   * @param {number} id - the DMX id of the first dimmer.
   * @param {number} fadeTime - rate of change in seconds per 100% change.
   * @param {number} [...] values - one or more dimmer values on scale 0 to 1.
   */
  setDimmer(id, fadeTime, ...values) {
    this.log(`setDimmer(${id}, ${fadeTime}, ${values.map(x => Math.round(x*255))})`);
    // Valid DMX addresses in our DMX universe are between 1 and 512.
    if (id <= 0 || id + values.length > 513) {
      throw new Error('Invalid DMX512 identifiers');
    }
    // We accept input values in the range 0 .. 1. Clamp all other values.
    values = values.map(x => Math.clamp(x, 0, 1));
    // Remember the values that the caller asked to set. These values could
    // differ from entries in this.values; that's particularly true in case
    // of dim-to-glow dimmers (see below).
    this.nominalValues = this.nominalValues.slice(0, id).concat(
      values).concat(this.nominalValues.slice(id + values.length));
    var i = id;
    for (const v of values) {
      const dest = Math.round(v*255);
      const diff = dest - this.values[i];
      const ms = fadeTime;
      const steps = Math.max(
        Math.round(Math.abs(diff)*fadeTime*1000/(255*this.fadeTimeStepMs)),
        this.values[i] <= 1 && dest > this.values[i]
          ? Math.round(this.powerOnTimeMs / this.fadeTimeStepMs)
          : 1);
      const delta = diff/steps;
      this.fader = this.fader.filter(x => x.id != i);
      this.fader.push({ id: i, dest: dest, steps: steps, delta: delta });
      ++i;
    }
    this.nextTickIn(0);
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
// How frequently to refresh dimmers during active fading (in milliseconds).
Enttec.prototype.fadeTimeStepMs = 50;
// Some dimmers don't power on, unless they are gradually ramped up.
Enttec.prototype.powerOnTimeMs = 250;

const Logger = {
  /**
   * Write a debug message to the system log, escaping common special
   * characters for better readability.
   * @param {string} msg - The message to be output.
   */
  log(msg) {
//  console.log(String(msg).replace(/\r/g, '\\r').replace(/\n/g, '\\n'));
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
