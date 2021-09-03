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

If you build with "make DEBUG=1", the program will be a lot more verbose.
Don't do that in production, though.
