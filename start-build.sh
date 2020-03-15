sudo make clean
sudo ./build.MUD-OS
touch malloc.c
touch mallocwrapper.c
touch applies_table.c
sudo make
sudo make install
