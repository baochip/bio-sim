# Simulation configurations

This directory contains various simulation configurations. Configuration files support
commands that simplify the configuration of the BIO, as well as allow for data generation
and monitoring. It's also possible to feed data into the simulation via a network socket,
and this method is recommended for more complicated test cases. See the `clients/` directory
for examples of this method.

The format of the file is `jsonc` because unlike standard json, these files support
human-readable comments through `//` and `/* */` constructs.