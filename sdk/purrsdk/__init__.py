"""purrsdk: everything the PURR Developer Kit does, as plain Python.

The `purr` command and PURR Studio are thin shells over this package. It needs only the
standard library to look at things (inspect, verify, serve, edit configs); making keys and
signing need the `cryptography` package, which the kit bundles.
"""

__version__ = "1.1.0"
