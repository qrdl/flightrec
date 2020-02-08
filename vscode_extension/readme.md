# Flight Recorder

This is VS Code Debugging extension for examining program run data, collected using [**Flight Recorder**](https://github.com/qrdl/flightrec).

## Using Flight Recorder Examine

* Install the **Flight Recorder** extension in VS Code.
* Switch to the debug viewlet and press the gear dropdown.
* Select the debug environment "Flight Recorder: Launch".
* Specify value for `program` parameter in `launch.json`
* If needed, specify values of `sourcePath` and `collectedData` parameters in `launch.json` (see below).
* Press the green 'play' button to start debugging.

## Supported `launch.json` parameters

`program` - executable to analyse

`collectedData` - file with collected data from run (`fr` file), by default - full pathname of executable + `.fr` extension

`sourcePath` - path to directory where sources are stored, by default - same path as for executable

## License

Flight Recorder and its components, including this extension, are licensed under [GNU Affero General Public License v3.0](https://www.gnu.org/licenses/agpl-3.0.en.html).