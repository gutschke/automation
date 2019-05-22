#!/usr/bin/env node

const Enttec   = require('./enttec');
const Fs       = require('fs');
const Http     = require('http');
const Lutron   = require('./lutron');
const Path     = require('path');
const Shell    = require('shelljs').exec;
const Traverse = require('traverse');
const WSSrv    = require('websocket').server;
const XML2JS   = require('xml2js').Parser();

class Automation {
  constructor() {
    this.enttec = new Enttec();
    this.radiora2 = Shell('./find-radiora2.sh', { silent: true }).stdout;
    this.lutron = new Lutron(_ => this.configureRadioRA2(), this.radiora2);
    this.dimmerState = { };
    this.htmlClients = [ ];
    this.keypads = { };

    // Define site-specific configuration options.
    this.dimmers = { };
    this.loads   = { };
    this.buttons = { };
    try {
      const site = require('./site.js');
      site.call(this);
    } catch (err) {
      console.log(err);
    }
  }

  dmx(e, flags, ...dimmers) {
    if (e.event === Lutron.Events.DOWN) {
      for (const dimmer of dimmers) {
        (dimmer <= (this.dimmers.MAXDIMTOGLOW || 0)
         ? this.enttec.setDimToGlow : this.enttec.setDimmer).
          call(this.enttec, dimmer, this.fadeTime,
               this.dimmerState[dimmer] && flags === this.Flags.TOGGLE ||
               flags === this.Flags.OFF ?
               this.dimmerState[dimmer] = 0 : this.dimmerState[dimmer] = 1);
      }
      this.buttons[e.id].lastDimmers = [...dimmers];
    }
  }

  dmxAdjustDimmer(e, increment) {
    clearTimeout(this.buttons[e.id].dimmerTimer);
    if (e.event === Lutron.Events.DOWN) {
      const fade = (rate) => {
        for (const dimmer of (this.buttons[e.id].lastDimmers || [ ])) {
          (dimmer <= (this.dimmers.MAXDIMTOGLOW || 0)
           ? this.enttec.setDimToGlow : this.enttec.setDimmer).
            call(this.enttec, dimmer, 0,
                 this.enttec.getDimmerValue(dimmer) + increment/rate);
        }
        this.buttons[e.id].dimmerTimer = setTimeout(() => fade, 50);
      };
      fade(1);
    } else {
      this.buttons[e.id].dimmerTimer = undefined;
    }
  }

  lutronFollow(event, followers) {
    for (let i = 0; i < followers.length; i += 2) {
      this.lutron.command(
        `#OUTPUT,${followers[i]},1,${
                   Number.parseFloat(event[3])*followers[i+1]/100}`).
        catch(_ => 0);
    }
  }

  buttonPressedOrLEDChanged(_, id, button, event, status) {
    console.log(`Button ${id}:${button} on ${this.keypads[id].label}, ` +
                `event type is ${event}${status === undefined ? '' :
                `, status is ${status}`}`);
    if ((event === 3 || event === 4) && this.buttons[id] !== undefined &&
        this.buttons[id][button] !== undefined) {
      // Button pressed
      this.buttons[id][button]({ id: id,
                                 button: button,
                                 event: event });
    } else if (event === 9 && this.keypads[id] !== undefined &&
               button > 80 && button <= 86) {
      // LED changed
      button -= 80;
      status = status === 1 ? 1 : 0;
      if (this.keypads[id].leds[button] !== status) {
        // The Lutron RadioRA2 controller has on occasion be observed to report
        // the wrong status change. Double-check a little while later.
        if (this.keypads[id].leds[button] !== undefined) {
          setTimeout(_ =>
                     this.lutron.command(`?DEVICE,${id},${button+80},${event}`).
                     catch(_ => 0),
                     500);
        }
        this.keypads[id].leds[button] = status;
        for (const client of this.htmlClients) {
          try {
            client.send(`${id},${button},${status === 1 ? 1 : 0}`);
          } catch (err) {
          }
        }
      }
    }
  }

  serveStaticFile(req, res) {
    // Hard-code common MIME types.
    const ext2Type = {
      '.css':  'text/css',
      '.html': 'text/html',
      '.jpg':  'image/jpg',
      '.js':   'text/javascript',
      '.json': 'application/json',
      '.png':  'image/png',
    };

    // Only serve files that are explicitly white-listed.
    const allowedFiles = [ '/', '/index.html' ];
    if (!allowedFiles.includes(req.url)) {
      res.writeHead(403, { 'Content-Type': 'text/html' });
      res.end(`<html><head><title>Error</title><body>Access denied
               </body></html>`, 'utf-8');
      return;
    }

    // Read the entire file into memory and then send it back to the browser.
    const filePath = req.url === '/' ? 'index.html' : '.' + req.url;
    Fs.readFile(filePath, (err, content) => {
      if (err) {
        res.writeHead(err === 'ENOENT' ? 404 : 500,
                      { 'Content-Type' : 'text/html' });
        res.end(`<html><head><title>Error</title></head><body>
                 Cannot serve: ${req.url}</body></html>`, 'utf-8');
      } else {
        res.writeHead(200, { 'Content-Type':
                             ext2Type[Path.extname(filePath)] || 'text/html' });
        res.end(content, 'utf-8');
      }
    });
  }

  async serveJSON(req, res) {
    while (!Object.entries(this.keypads).length) {
      await new Promise(r => setTimeout(r, 1000));
    }
    if (req.url === '/keypads.json') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(this.keypads), 'utf-8');
    } else {
      res.writeHead(404, { 'Content-Type': 'text/html' });
      res.end('<html><head><title>Error</title><body>Not found</body></html>',
              'utf-8');
      return;
    }
  }

  async subscribe(connection) {
    // Tell the web client any time a button is pressed or a LED changes state.
    this.htmlClients.push(connection);

    // Send the cached state of LEDs.
    for (const id in this.keypads) {
      for (let i = 1; i <= 6; ++i) {
        if (this.keypads[id].leds[i] !== undefined) {
          connection.send(`${id},${i},${this.keypads[id].leds[i]}`);
        }
      }
    }
  }

  retrieveInitialConfiguration() {
    // Obtain the site's database in XML format and extract useful information.
    Http.get(`http://${this.radiora2}/DbXmlInfo.xml`, res => {
      let data = '';
      let kp = { };
      res.setEncoding('utf8');
      res.on('data', chunk => data += chunk);
      res.on('error', () => 0); /***/
      res.on('end', () => XML2JS.parseString(data, (err, result) => {
        if (!err) {
          // Traverse the XML database and find all keypads.
          Traverse(result).forEach(((that, kp) => { return function(o) {
            if (o.DeviceType !== undefined &&
                (o.DeviceType.endsWith('_KEYPAD') ||
                 o.DeviceType === 'MAIN_REPEATER')) {
              console.log(o);
              console.log(this.parent.node.Components[0].Component);
              // Extract the keypad id and label.
              const id = parseInt(o.IntegrationID);
              kp[id] = { label: o.Name, leds: { }, buttons: { },
                         loads: { } };
              // Find all buttons and their labels.
              for (const button of this.parent.node.Components[0].Component) {
                if (button.Button === undefined) continue;
                const num = button.$.ComponentNumber;
                kp[id].buttons[num] = button.Button[0].$.Engraving;
                if (num >= 18) continue;
                kp[id].leds[num] = button.Button[0].$.LedLogic;
                try {
                  for (const preset of button.Button[0].Actions[0].Action[0].
                                              Presets[0].Preset[0].
                                              PresetAssignments[0].
                                              PresetAssignment) {
                    if (typeof kp[id].loads[num] === 'undefined') {
                      kp[id].loads[num] = { };
                    }
                    kp[id].loads[num][preset.IntegrationID[0]] =
                      parseInt(preset.Level[0])/100;
                  }
                } catch (_) { }
              }
              console.log(kp[id]);
              // Monitor changes to buttons and LEDs.
              that.lutron.monitor('~DEVICE', id, (...args) =>
                that.buttonPressedOrLEDChanged.apply(that, args));
              // Request initial LED status.
              if (o.DeviceType.endsWith('_KEYPAD')) {
                for (let i = 1; i <= 6; ++i) {
                  that.lutron.command(`?DEVICE,${id},${80+i},9`).
                    catch(_ => 0);
                }
              }
            }
          };})(this, kp));
          this.keypads = kp;
        }}));});
  }

  async configureRadioRA2() {
    // Tell the RadioRA2 controller that we want to know about all events.
    for (const type of [ 2, 3, 4, 5, 6, 8, 17, 18, 23 ]) {
      this.lutron.command(`#MONITORING,${type},1`).
        catch(_ => 0);
    }

    // For all dummy loads, monitor status changes.
    for (const id in this.loads) {
      this.lutron.monitor('~OUTPUT', id , 1,
                          (...args) => this.loads[id].cb(args));
    }
  }

  async main() {
    this.retrieveInitialConfiguration();

    // Create web server that can serve static files and dynamic JSON data.
    const http = Http.createServer((req, res) => {
      if (req.url.endsWith('.json')) {
        this.serveJSON(req, res);
      } else {
        this.serveStaticFile(req, res);
      }}).listen(8080)
          .on('clientEror', (err, socket) => socket.end());

    // Add a websocket server that bridges between browser and RadioRA2.
    const wsServer = new WSSrv({ httpServer: http,
                                 autoAcceptConnections: false })
          .on('request', (req) => {
            let connection = req.accept('', req.origin);
            connection.on('message', (msg) => {
              if (msg.utf8Data === 'subscribe') {
                this.subscribe(connection);
              } else if (msg.utf8Data.startsWith('#DEVICE,')) {
                this.lutron.command(msg.utf8Data).catch(_ => 0);
              }
            }).on('error', () => {
              connection.close();
              this.htmlClients = this.htmlClients.filter(v => v !== connection);
            }).on('close', () =>
              this.htmlClients =this.htmlClients.filter(v => v !== connection));
          });

    // Run forever, responding to events.
    new Promise(_ => 0);
  }
}
Automation.prototype.fadeTime = .4;
Automation.prototype.Flags = {
  OFF: 1, ON: 2, TOGGLE: 3, SCENE: 4,
  LED: 256, DIM: 512, MASK: 255,
};

if (require.main === module) {
  new Automation().main();
}
