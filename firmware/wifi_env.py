# Passes WiFi credentials from environment variables into the build, so they never
# live in a file. Encoded as byte lists (e.g. 104,105,0) so spaces and special
# characters survive the compiler command line.

import os

Import("env")


def as_bytes(value):
    return ",".join(str(b) for b in value.encode("utf-8") + b"\0")


env.Append(CPPDEFINES=[
    ("WIFI_SSID_BYTES", as_bytes(os.environ.get("WIFI_SSID", ""))),
    ("WIFI_PASS_BYTES", as_bytes(os.environ.get("WIFI_PASS", ""))),
])
