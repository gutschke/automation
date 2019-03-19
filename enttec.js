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
    this.reader = null;
    this.waitingForOpen = [ ];
    this.oldData = '';
    this.values = Array(this.minNumberOfSlots).fill(0);
    this.refresh = () => {
      if (this.refresh !== null) {
        this.sendDMXPackage(this.values);
        // DMX likes to receive all dimmer values at least once per second.
        // According to the Wikipedia page, that also ensures that several
        // other timing parameters stay within the expected range.
        setTimeout(this.refresh, 1000);
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
    const reader = this.reader;
    this.reader = null;
    const err = new Error('Object destroyed');
    for (const w of waitingForOpen) {
      try {
        w.reject(err);
      } catch (err) {
      }
    }
    try {
      this.reader.reject(err);
    } catch (err) {
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
      this.waitingForOpen.push({ resolve: resolve, reject: reject });
      if (this.waitingForOpen.length > 1) {
        return;
      }
      function err(msg) {
        const error = new Error(msg);
        const waitingForOpen = this.waitingForOpen;
        this.waitingForOpen = [ ];
        const reader = this.reader;
        this.reader = null;
        const s = this.serial;
        this.serial = null;
        if (s) s.close();
        for (const w of waitingForOpen) {
          try {
            w.reject(error);
          } catch (err) {
          }
        }
        try {
          reader.reject(error);
        } catch (err) {
        }
      }
      const serial = new SerialPort(this.port, () => {
        const waitingForOpen = this.waitingForOpen;
        this.waitingForOpen = [ ];
        this.serial = serial;
        for (const w of waitingForOpen) {
          try {
            w.resolve(serial);
          } catch (err) {
          }
        }
      }).on('data', data => {
        const r = this.reader;
        this.reader = null;
        if (r && r.resolve) {
          r.resolve(data);
        } else {
          this.oldData = this.oldData + data;
        }
      }).on('error', () => { err('serial port error'); })
        .on('end',   () => { err('serial port closed'); })
        .setEncoding('binary');
    });
  }

  /**
   * Wrap the SerialPort.write() method into a Promise. Also, ensure that the
   * port is open, if it hadn't previously been opened. This is useful after
   * temporary I/O errors (e.g. device unplugged from USB) to retry operations.
   * @param {string} data - data that should be written to Enttec DMX widget.
   */
  async write(data) {
    await this.openPort();
    return new Promise((resolve, reject) => {
      this.serial.write(data, 'binary', err => {
        if (err) {
          reject(err);
        } else {
          resolve();
        }
      });
    });
  }

  /**
   * Read data from the Enttec DMX widget. This method is only used for
   * debugging purposes right now. TODO: Learn about packet framing and
   * reassemble packets.
   */
  async read() {
    if (this.oldData !== '') {
      const s = this.oldData;
      this.oldData = '';
      return s;
    }
    const promise = new Promise((resolve, reject) => {
      this.reader = { resolve: resolve, reject: reject };
    });
    await this.openPort();
    return promise;
  }

  /**
   * @private
   * Send one DMX dataset.
   * @param {number[]} data - bytes to be send.
   */
  async sendDMXPackage(data) {
    this.log(`sendDMXPackage(${data})`);
    if (data.length < this.minNumberOfSlots || data.length > 512) {
      throw new Error('Invalid DMX package');
    }
    // Estimate how long it'll take to transmit our package at 250kbaud with
    // an 8N1 encoding.
    const time = data.length*11/250;
    // Send multiple copies of the same package in order to ensure that we
    // spend at least 5ms before making other changes.
    const reps = Math.max(1, Math.round(this.minTimeMs/time));
    this.log(`${data} * ${reps}`);
    // Add the required framing for the Enttec DMX widget. 0x7E and 0xE7 are
    // markers to help find packages. "6" is the command for sending DMX
    // packages. And we encode the length in little-endian order.
    const buffer = Buffer.from([].concat(...Array(reps).fill(
      [ 0x7E, 6, data.length & 0xFF, (data.length >> 8) & 0xFF].concat(
      data).concat(
      [ 0xE7 ]))));
    return this.write(buffer, 'binary');
  }

  /**
   * Uses the Enttec DMX widget to set the brightness of one or more DMX-
   * addressable dimmers.
   * @param {number} id - the DMX id of the first dimmer.
   * @param {number} [...] values - one or more dimmer values on scale 0 to 255.
   */
  async setDimmer(id, ...values) {
    this.log(`setDimmer(${id}, ${values})`);
    if (id <= 0 || id + values.length > 512) {
      throw new Error('Invalid DMX512 identifiers');
    }
    if (this.values.length < id) {
      // Sending values for dimmers that we have never previously controlled.
      // Add any necessary padding.
      this.values = this.values.concat(
        Array.from({length: id-this.values.length}, () => 0))
        .concat(values);
    } else {
      // Sending values for dimmers that we have previously controlled. Update
      // the cached data.
      this.values = this.values.slice(0, id).concat(
        values).concat(
        this.values.slice(id + values.length));
    }
    return this.sendDMXPackage(this.values.slice(0, Math.max(
      this.minNumberOfSlots, id + values.length)));
  }

  /**
   * Sets a dimmer's brightness, respecting dim-to-glow characteristics.
   * The light fixture actually has two DMX ids; the first one dims a string
   * of cold LEDs, and the second one dims a string of warm LEDs. By adjusting
   * the mix, we can shift color temperature while dimming brightness.
   * @param {number} dimmer - the DMX id of the first (i.e. cold) dimmer.
   * @param {number} [...] values - the desired brightness on a scale of
   *                                0 to 1.0.
   */
  async setDimToGlow(dimmer, ...values) {
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
    const warmCold = [ dimmer ];
    for (const value of values) {
      const quadratic = (v, initialRate, finalRate, weight) =>
        Math.clamp(Math.round(
          ((finalRate-initialRate)/(initialRate+finalRate)*v*v +
           2*initialRate/(initialRate+finalRate)*v)*weight*255), 0, 255);
      const v = Math.pow(value, 1.2);
      warmCold.push(/* cold LEDs */quadratic(v, 0.5, 1.3, 1));
      warmCold.push(/* warm LEDs */quadratic(v, 1.3, 0.5, .6));
    }
    return this.setDimmer.apply(this, warmCold);
  }
}
// Minimum number of DMX slots needed to guarantee that we never send faster
// than the minimum required time in between break conditions on the serial
// line.
Enttec.prototype.minNumberOfSlots = 24;
// Restrict updates to at most one every 5ms. Our dimmers sometimes drop
// DMX packets if we send faster than that.
Enttec.prototype.minTimeMs = 5;

const Logger = {
  /**
   * Write a debug message to the system log, escaping common special
   * characters for better readability.
   * @param {string} msg - The message to be output.
   */
  log(msg) {
    console.log(msg.replace(/\r/g, '\\r').replace(/\n/g, '\\n'));
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
