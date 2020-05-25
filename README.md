# Flight recorder

![Build/Test](https://github.com/qrdl/flightrec/workflows/Build/Test/badge.svg?branch=master)
[![Language grade: C/C++](https://img.shields.io/lgtm/grade/cpp/g/qrdl/flightrec.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/qrdl/flightrec/context:cpp)
[![Coverity Scan](https://scan.coverity.com/projects/20876/badge.svg)](https://scan.coverity.com/projects/qrdl-flightrec)

## Description
Flight Recorder (Flightrec for short) allows to record client program execution and examine it later. It consists of three building blocks:
* Record - the part that records program execution and stores all steps, registers and memory changes
* Examine - debug server, it allows to access logged data using DAP protocol
* VS Code extension - debugging extension for VS Code that communicates with Examine

## Features
Flightrec supports most of the standard debugger features, such as running till the breakpoint hit, stepping over, inside and out, but it also allows to run backwards and step back. It is extremely useful when trying to deal with hard-to-debug problems, when you just set the breakpoint at the line where error was detected, and step back until you find the root cause, examining the changes of variables.

And because you are examining the recorded run, you don't need to wait for the client program to execute - you jump to the breakpoint immediately.

## Limitations

### Hardware
Flightrec was tested only on x86-64 architecture, however it should be possible to support other architectures as well. Flight recorder benefits from using x86-64 vector instructions (SSE2/AVX2/AVX512, depending on availability), so running it on different platforms may be significantly slower.

### OS
Flightrec supports only 64-bit Linux and it uses some Linux-specific (non-POSIX) APIs for monitoring memory. There are no plans to support other OSes.

Linux kernel must have `CONFIG_PROC_PAGE_MONITOR` switched on to generate `/proc/<pid>/pagemap` file, which is default for most distribution, but not for WLS (Windows Subsystem for Linux), therefore Flightrec doesn't work under WSL. Also Linux kernel must support eBPF (standard for all recent kernel versions).

### Languages and compilers
For now Flightrec supports only client binaries, compiled from C using GCC compiler. There are plans to support other compilers and languages, and there were some encouraging tests with Go using native Go compiler.

### Debug info format
Flightrec supports only ELF client binaries with DWARF2 debug information, therefore it is important to compile traced programs with `-gdwarf2` option. There are plans to support DWARF4/5 debug formats as well.

### Optimisation
With high optimisation settings GCC may create complex locations for variables, such as "first 8 bytes in register, reminder in memory at certain address", and it is not supported by Flightrec.
Therefore is recommended to compile client without optimisation.

### Memory management
Flightrec intercepts calls to `malloc`/`free` family of functions in order to monitor memory changes, therefore if child process uses custom memory management, it can interfere with Flightrec's logic.

### Threads
Although Flightrec is multi-threaded application, it doesn't support multi-threaded clients, it means that only main thread of the client is analysed. There are plans to support multi-threaded clients in the future.

## Pre-requisites

### Record and Examine
To build Flightrec make sure you have:
* [GNU gperf](https://www.gnu.org/software/gperf/) (Linux package name _gperf_)
* [Flex](https://github.com/westes/flex) (Linux package name _flex_)
* [GNU Bison](https://www.gnu.org/software/bison/) (Linux package name _bison_)

Also following libraries are required (with Ubuntu package names, other distributions may use different names):
* libelf (`libelf-dev`)
* libdwarf (`libdwarf-dev`)
* sqlite3 (`libsqlite3-dev`)
* json-c (`libjson-c-dev`) version 1.13 or above (versions below 1.13 aren't compatible with Flightrec)
* libbpf (`libbpfcc-dev`)

### VSCode extension
To package VSCode extension you need to have [Node.js](https://nodejs.org/) installed, with [vsce](https://github.com/microsoft/vscode-vsce).
To install `vsce` run `npm install vsce`.

## Building
To build Flightrec run `make` in Flightrec's root directory. Binaries `fr_record`, `fr_preload.so` will be copied into Flightrec's root directory. VSCode debugging extension will be created in `vscode_extension` directory.

## Installing
Run `make install` in Flightrec's root directory. Please note that installing Flightrec Recorder requires root access due to the fact that `fr_record` binary runs as `root` because otherwise it cannot load eBPF program inside Linux kernel (eBPF is used to detect memory changes and certain memory-related syscalls, such as `memmap` and `brk`).


## Recording run

### Building binary to analyse
Client program to be analysed by Flightrec must be built with DWARF2 debug information, to do it add `-g3 -gdwarf2` options to GCC compilation command. Make sure GCC option list doesn't contain any optimisation options (`-Ox`).

### Processing
Run `fr_record [<options>] -- <client> [<client options>]` to record the run. As a result Flightrec creates file with name of client and `fr` extension, this file is used later by Examine.

Run under Recorder is much slower than regular run, therefore it is recommended to limit the number of recorded steps. To do it some translation units can be excluded from analysis, as a result user won't be able to step inside the calls for functions, located in these units. For example if program is directly linked with SQLite3 (not as external library), it can be excluded with `-x sqlite3.c` option. `-x` option specifies units to exclude (blacklist), while `-i` option specifies units in to include (whitelist). If whitelist is specified, blacklist is ignored. Both `-x` and `-i` options can occur several times.

Units, compiled without debug information, excluded from analysis automatically.

By default Flightrec processes only source files that are located in and under current directory, however it is possible to specify alternative path for sources with `-p` option.

Example:
`fr_record -p ../src -x sqlite3.c -- ./foo foo_param1 foo_param2`

## Examining data
To examine recorded data create debug configuration in `launch.json` file in VS Code, using "Flight Recorder: Launch" template, Make sure this configuration contains correct path to client in `program` parameter.

By default Flightrec considers that file with collected data from run (`fr` file) is located in the same directory where the client is, and sources are under current working directory, however it is possible to specify different locations for both, using, `sourcePath` and `collectedData` parameters.

Example of such configuration:
```json
{
    "name": "Flight Recorder: Launch",
    "type": "flightrec",
    "request": "launch",
    "program": "${workspaceFolder}/build/foo",
    "sourcePath": "${workspaceFolder}/src",
    "collectedData": "${workspaceFolder}/foo.fr",
    "stopOnEntry": true
}
```

## License
This project is released under GNU Affero General Public License v3.0. See [LICENSE](LICENSE) for details.
