# qjump-tc
QJump Traffic Control Module - The heart of QJump

To build, run "make" in the source directory. Make sure that you have kernel headers installed.

To use, run:
sudo insmod sch_qjump.ko

The module supports several paramters. 
- verbose (int):    (in the range 0-4) which varies how verbose the module output is
- bytesq (int):     sets the number of bytes per epoch. This is the "P" parameter from the QJump equation
- timeq (int):      sets the time (in microseconds) per epoch. This is the result of the QJump equation. 
- pXrate (int):     sets the the "f" value as an integer multiple of the base rate given by bytesq/timeq.
- autolass (bool):  turns on the "autoclassifier" which degrades an application until it reaches the right QJump level
 
