# automation

A daemon program that integrates third party components with a Lutron
RadioRA2 system. In particular, this means DMX light fixtures that are
not natively supported by Lutron. But there also is support for GPIO
pins, as found on a Raspberry Pi.

## Dependencies

* https://github.com/fmtlib/fmt
* https://github.com/nlohmann/json
* https://github.com/zeux/pugixml

## Configuration

You need to provide a "site.json" file to customize the system to your local
needs. Without this configuration information, there is nothing for the
program to do.

If you build with "make DEBUG=1", the program will be a lot more verbose.
Don't do that in production, though.
