const net = require('net');

/** Class representing a connection to the Lutron RadioRA2 controller */
class Lutron {
  /**
   * Create a new connection to the given host. Logs in with the username
   * and password when needed. In case of error, fails the operation and
   * then re-opens a connection on the next operation.
   * @param {string} gateway - Hostname or ip address.
   * @param {string} [username] - The account name to use for authentication.
   * @param {string} [password] - The account password.
   */
  constructor(gateway, username, password) {
    this.gateway = gateway;
    this.username = username || 'lutron';
    this.password = password || 'integration';
    this.socket = null;
    this.oldData = '';
    this.monitors = [ ];
    this.active = 0;
  }

  /**
   * @private
   * Return the open socket object or open a new connection and log in the
   * user.
   * @return {Lutron.AsyncSocket} A socket object or null.
   */
  async login() {
    if (!this.socket) {
      let s;
      try {
        s = new Lutron.AsyncSocket(23, this.gateway, this.Prompts,
                                   (l) => this.processMonitors(l));
        for (; ; s.destroy()) {
          let p = await s.read();
          if (p === this.Prompts[2]) break;
          if (p !== this.Prompts[0]) continue;
          s.write(this.username + '\r\n');
          p = await s.read();
          if (p === this.Prompts[2]) break;
          if (p !== this.Prompts[1]) continue;
          s.write(this.password + '\r\n');
          if (await s.read() !== this.Prompts[2]) continue;
          break;
        }
        this.socket = s;
      } catch (err) {
        // Do nothing
      }
    }
    return this.socket;
  }

  /**
   * @private
   * Processes applicable monitors, if any.
   * @param {string} line - next line read from controller.
   */
  processMonitors(line) {
    const parts = line.split(',');
    for (let monitor of this.monitors) {
      try {
        if (parts[0] === monitor.command) {
          for (let i = 0; i < monitor.args.length; ++i) {
            if (parts[i+1] !== monitor.args[i]) break;
            if (i === monitor.args.length-1) {
              setTimeout(() => monitor.cb(...parts.map(x => parseInt(x))), 0);
            }
          }
        }
      } catch (err) {
        // Do nothing
        this.log(err);
      }
    }
  }

  /**
   * @private
   * Reads a line from the Lutron RadioRA2 controller. Before returning to
   * the caller, invokes any pending monitors that apply.
   * @return {string} The next line returned from the controller.
   */
  async read() {
    const line = await this.socket.read();
    return line;
  }

  /**
   * Send a command to the Lutron RadioRA2 controller and wait for it to
   * complete. Throws an error, if anything went wrong.
   * @return {string} Response from controller, if any.
   */
  async command(cmd) {
    console.log(`command(${cmd})`);
    try {
      while (this.active) {
        await new Promise(r => setTimeout(r, 20+Math.random()*40));
      }
      ++this.active;
      if (this.socket)
        this.socket.setIdle(!this.active);
      await this.login();
      let err, rc;
      this.socket.write(cmd + '\r\n');
      for (;;) {
        let s = await this.read();
        if (cmd.startsWith('?') &&
            s.startsWith('~' + cmd.substr(1) + ',')) {
          rc = s.substr(cmd.length + 1);
        } else if (s.startsWith('~ERROR')) {
          err = new Error(s);
        } else if (s === 'is an unknown command') {
          err = new Error(`${cmd} ${s}`);
        } else if (s === this.Prompts[2] /* GNET> */) {
          if (rc === undefined) {
            rc = true;
          }
          break;
        }
      }
      if (err) {
        throw err;
      } else if (rc) {
        return rc;
      }
    } catch (e) {
      this.log(e);
      if (this.socket) {
        let s = this.socket;
        this.socket = null;
        s.destroy();
      }
      throw e;
    } finally {
      --this.active;
      if (this.socket)
        this.socket.setIdle(!this.active);
    }
  }

  /**
   * Registers a new callback to be invoked whenever the controller sends a
   * message that matches "command" followed by a subset of arguments.
   * @return {Object} An object that can be used to remove the monitor.
   */
  monitor(command, ...args /*, cb */) {
    const cb = args.pop();
    const m = { command: command, args: args.map(x => `${x}`), cb: cb };
    this.monitors.push(m);
    return m;
  }

  /**
   * Removes a previously registered monitor.
   * @param {Object} The return value from a previous call to "monitor()".
   */
  removeMonitor(monitor) {
    this.monitors = this.monitors.filter(m => m !== monitor);
  }
}
Lutron.prototype.Prompts = [ 'login: ', 'password: ', 'GNET> ' ];
Lutron.Events = { DOWN: 3, UP: 4 };

/**
 * Wrapper around net.Socket, so that it can be use with "await" instead
 * of with callbacks. Only a small subset of socket methods are exposed.
 */
Lutron.AsyncSocket = class {
  /**
   * Create a socket object that allows to connect to a Lutron RadioRA2
   * controller and that knows how to break responses into lines. It also
   * recognizes the different command prompts that we are likely to see.
   * @param {number} port - Port number to connect to.
   * @param {string} host - Hostname or IP address to connect to.
   * @param {string[]} prompts - List of prompts that should be detected.
   */
  constructor(port, host, prompts, processMonitors) {
    this.port = port;
    this.host = host;
    this.prompts = prompts;
    this.processMonitors = processMonitors;
    this.socket = undefined;
    this.reader = null;
    this.oldData = '';
    this.idle = true;
  }

  /**
   * @private
   * Returns the next line of data that has been received or an empty string.
   * Breaks data into lines and makes sure that a full line has been received.
   * All lines are terminated by '\r\n', but returned data is always stripped
   * of these characters.
   * Also, recognizes the global list of prompts provided to the constructor.
   * If a prompt has been read at the beginning of a new line, return the
   * prompt to the caller without waiting for any additional characters.
   * @return {string} The next line or next prompt that needs processing.
   */
  nextLine(data) {
    data = this.oldData + data;
    let d = data.replace(/^\s*/, '');
    for (let p of this.prompts) {
      if (d.startsWith(p)) {
        this.oldData = d.substr(p.length);
        this.processMonitors(p);
        this.log(`read() -> "${p}"`);
        return p;
      }
    }
    let eol = data.indexOf('\r\n');
    if (eol >= 0) {
      this.oldData = data.substr(eol + 2);
      data = data.substr(0, eol);
      this.processMonitors(data);
      this.log(`read() -> "${data}"`);
      return data;
    } else {
      this.oldData = data;
      return '';
    }
  }

  /**
   * @private
   * Whenever data has been received by the socket, either buffer it or
   * fulfill any currently pending promise. There can only ever be a single
   * promise that is pending. The promise only ever receives a single line
   * at a time.
   */
  onData(data) {
    if (data !== null) {
      // The controller has a bug that sometimes inserts \0 characters
      // at the end of the prompt.
      data = String(data).replace(/\0/g, '');
    }
    if (this.reader || this.idle) {
      let r = this.reader;
      this.reader = null;
      data = this.nextLine(data);
      if (data.length > 0) {
        if (r && r.resolve) {
          r.resolve(data);
        }
      }
    } else {
      this.oldData = this.oldData + data;
    }
  }

  /**
   * Set the connection as idle (i.e. discard all unsolicited data) or as
   * busy (i.e. collect all data, so that it can be returned from read() calls).
   * @param {boolean} idle - indicate whether the connection is idle or busy.
   */
  setIdle(idle) {
    this.idle = idle;
    if (idle) {
      this.oldData = '';
    }
  }

  /**
   * @private
   * Opens the connection to the Lutron RadioRA2 controller and sets up the
   * mapping between callbacks and promises. It is OK to call this method with
   * the socket already open. In fact, it is recommended to always call
   * before each operation, as it will re-open sockets if necessary.
   */
  async openSocket() {
    if (this.socket !== undefined) return;
    this.socket = net.connect(this.port, this.host);
    const that = this;
    return new Promise((resolve, reject) => {
      function err(msg) {
        that.log(`onError("${msg}")`);
        const error = new Error(msg);
        if (reject) {
          const r = reject;
          reject = null;
          r(error);
        } else {
          const s = that.socket;
          that.socket = null;
          if (s) s.destroy();
        }
        if (that.reader) {
          const r = that.reader;
          that.reader = null;
          if (r.reject) {
            r.reject(error);
          }
        }
      }
      this.socket.on('error',   ()  => err('socket error'))
                 .on('end',     ()  => err('connection closed'))
                 .on('data',    (d) => this.onData(d))
                 .on('connect', ()  => {
                   this.idle = false; // Waiting for login prompt
                   reject = null;
                   resolve(); });
    });
  }

  /**
   * Reads the next line from the Lutron RadioRA2 controller. This method
   * always returns a promise that can be resolved to retrieve the line.
   * @return {Promise} A promise that returns the next line.
   */
  async read() {
    const data = this.nextLine('');
    if (data.length > 0) {
      return data;
    }
    if (this.socket === null) {
      throw new Error('connection closed');
    }
    const promise = new Promise((resolve, reject) => {
      this.reader = { resolve: resolve, reject: reject };
    });
    await this.openSocket();
    return promise;
  }

  /**
   * Sends data to the Lutron RadioRA2 controller. The caller has to append
   * '\r\n' if applicable.
   * @param {string} data - The string to be sent to the controller.
   */
  write(data) {
    this.log(`write("${data}")`);
    if (!this.socket) {
      throw new Error('connection closed');
    }
    this.socket.write(data);
  }
  /**
   * Destroys the current socket and rejects any pending promise.
   */
  destroy() {
    this.log('destroy()');
    if (this.socket) {
      const s = this.socket;
      this.socket = null;
      if (s) s.destroy();
    }
    if (this.reader) {
      const r = this.reader;
      this.reader = null;
      if (r.reject) {
        r.reject(err);
      }
    }
  }
}

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
Object.assign(Lutron.prototype, Logger);
Object.assign(Lutron.AsyncSocket.prototype, Logger);

module.exports = Lutron;
