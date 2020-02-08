##########################################################################
#
#  File:       	Makefile
#
#  Project:    	Flight recorder (https://github.com/qrdl/flightrec)
#
#  Descr:      	VSCode extension makefile
#
#  Notes:	It produces VSIX file which needs to be installed in
#		VSCode as extention
#
##########################################################################
#
#  Copyright (C) 2017-2020 Ilya Caramishev (ilya@qrdl.com)
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU Affero General Public License as
#  published by the Free Software Foundation, either version 3 of the
#  License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU Affero General Public License for more details.
#
#  You should have received a copy of the GNU Affero General Public License
#  along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
##########################################################################
VSIX = flightrec-0.1.0.vsix
.PHONY: all clean install uninstall reinstall

all: $(VSIX)

$(VSIX): package.json readme.md fr_examine
	vsce package

install:
	code --install-extension $(VSIX)

uninstall:
	code --uninstall-extension qrdl.flightrec

reinstall: uninstall clean $(VSIX) install

clean:
	rm -f $(VSIX)

