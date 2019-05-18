const Gpio = require('pigpio').Gpio;

/**
 * The Relay object can operate relays and checking the current state of
 * sensors.
 * As we rely on the ability to set the pull up/down resistors, this code
 * must be executed as root.
 */
class Relay {
  /** Constructor for the Relay object.
   * @param {number} relay - pin number for relay output.
   * @param {number} sensor - pin number for sensor input.
   */
  constructor(relay, sensor) {
    this.relay = relay && new Gpio(relay, {
      mode: Gpio.INPUT, pullUpDown: Gpio.PUD_DOWN });
    this.sensor = sensor && new Gpio(sensor, {
      mode: Gpio.INPUT, pullUpDown: Gpio.PUD_UP, alert: true });
    if (this.sensor) {
      // 500ms filter. Some of the sensors are rather bouncy.
      this.sensor.glitchFilter(300000);
    }
  }

  /** Monitor for state changes in the sensor.
   * @param {function} cb - callback function for state change notifications.
   */
  monitor(cb) {
    this.sensor.on('alert', (level, tick) => cb(level, tick));
  }

  isOpen() {
    return this.sensor.digitalRead();
  }

  /** Simulates pushing a momentary push button.
   */
  async pushButton() {
    console.log(`Pushing button ${this.relay.gpio}`);
    // There are two types of devices that we are trying to drive. For
    // keyfob-style remote controls, we turn the digital output to on. This
    // turns the output to 3.3V. It'll be read by a high-Ohm input on the
    // keyfob and interpreted as a button push.
    // For relay boards, the logic is reverse. A 3.3V signal will be
    // interpreted as off, and a 0V signal will be interpreted as on.
    // Fortunately though, the input is relatively low-Ohm (~1kΩ). When the
    // pin is configured for Gpio.INPUT it doesn't actually read as 0V, even
    // when the pull-down resistor is activated.
    // This allows us to have a common way to signal "off". Set the pin to
    // input and activate the pull-down resistor. For "on", we have to try
    // both outputting 3.3V and 0V, though. We only need to hold the value
    // for a short amount of time, when simulating button presses, though.
    this.relay.digitalWrite(0);
    this.relay.mode(Gpio.OUTPUT);
    // The relay board now reads an "on" condition.
    await new Promise(r => setTimeout(r, 100))
    // Next, we signal an "on" condition to the keyfob remote
    this.relay.digitalWrite(1);
    await new Promise(r => setTimeout(r, 300))
    // Return pin to the "off" condition, by relying on the pull-down resistor.
    this.relay.mode(Gpio.INPUT);
    this.relay.digitalWrite(0);
  }
}

module.exports = Relay;
