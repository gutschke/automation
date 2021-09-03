# automation

A daemon program that integrates third party components with a Lutron
RadioRA2 system. In particular, this means DMX light fixtures that are
not natively supported by Lutron. But there also is support for GPIO
pins, as found on a Raspberry Pi.

## Dependencies

* https://github.com/fmtlib/fmt
* https://github.com/nlohmann/json
* https://github.com/zeux/pugixml
* https://github.com/warmcat/libwebsockets

## Configuration

You need to provide a "site.json" file to customize the system to your local
needs. Without this configuration information, there is nothing for the
program to do.

Alternatively, most of the information that would normally be part of the
"site.json" file can also be encoded in the labels that you assign to
keypads and devices in the Lutron software. See "site.json.sample" for
some common configuration options and how you would encode them to strings
that can be used on labels.

## Getting started

1. Use the Lutron software to add a new dimmer, but don't pair it with any
   physical device. This is a virtual placeholder device that stands in for
   your DMX fixture.
2. Edit the "Zone Name" and append a ":" colon followed by the numeric DMX
   id of your fixture. See "site.json.sample" for more advanced DMX-related
   parameters.
3. Assign the dimmer to one of your keypad buttons just as what you would
   do with native Lutron devices.
4. Upload the new configuration to the Lutron controller and disregard the
   warning about the unassigned dimmer that you created.
5. Create a "site.json" file that has the username and password for your
   Lutron device, if you changed it from the default values.
6. Start the "automation" binary. Initializing the connection with the Lutron
   device can take up to about one minute.
7. Your DMX fixture should now operate just like any native Lutron output
   devices.
8. Check "site.json.sample" for exmples of more advanced configuration options.
9. If things don't work right away, refer to the next paragraph.

## Diagnostics

If you build with "make DEBUG=1", the program will be a lot more verbose.
That's usually what you should do when trouble-shooting. Don't do that in
production though, as debug mode disables the watchdog mode, disables
automatic restart when configuration changes, and enables a remote DMX server.
This all makes debugging easier but isn't appropriate for daily use.
