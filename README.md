# qjump-tc
QJump Traffic Control Module - The heart of QJump

To build, run "make" in the source directory. Make sure that you have kernel headers installed.

To use, run with optional paramters decribed below. eg:
sudo insmod sch_qjump.ko verbose=4

The kernel module supports several paramters. 
- verbose (int):    (in the range 0-4) which varies how verbose the module output is
- bytesq (int):     sets the number of bytes per epoch. This is the "P" parameter from the QJump equation
- timeq (int):      sets the time (in microseconds) per epoch. This is the result of the QJump equation. 
- pXrate (int):     sets the the "f" value for QJump level X. Value is set as an integer multiple of the base rate given by bytesq/timeq. For example, setting p2rate to 5 indicates that the third highest priority level (p2) will receive 5x the number of bytes per epoch the highest prioirty (p0).
- autolass (bool):  turns on the "autoclassifier" which degrades an application until it reaches the right QJump level

To check that the module has inserted properly use dmesg. eg:
dmesg

[ 2379.582475] sch_qjump: module license 'BSD' taints kernel.  
[ 2379.582479] Disabling lock debugging due to kernel taint  
[ 2379.583112] QJump[4]: Init module  
[ 2379.583126] QJump[4]: Calculating CPU speed 0  
[ 2380.583698] QJump[4]: CPU is running at 2799405818 cyles per second  
[ 2380.583701] QJump[4]: Calculating CPU speed 1  
[ 2381.584274] QJump[4]: CPU is running at 2799407672 cyles per second  
[ 2381.584277] QJump[4]: Calculating CPU speed 2  
[ 2382.584850] QJump[4]: CPU is running at 2799407705 cyles per second  
[ 2382.584853] QJump: Module parameteres:  
[ 2382.584853] -------------------------------  
[ 2382.584854] QJump: timeq=100us  
[ 2382.584854] QJump: bytesq=100B  
[ 2382.584855] QJump: p7rate=10000  
[ 2382.584856] QJump: p6rate=0  
[ 2382.584856] QJump: p5rate=0  
[ 2382.584856] QJump: p4rate=1000  
[ 2382.584857] QJump: p3rate=100  
[ 2382.584857] QJump: p2rate=10  
[ 2382.584858] QJump: p1rate=5  
[ 2382.584858] QJump: p0rate=1  
[ 2382.584859] -------------------------------  
[ 2382.584859]   

After inserting the module, you can bind qjump to an ethernet port using TC. eg:  
sudo tc qdisc add dev eth0 root qjump

You can then check that the module is working e.g:
dmesg

[ 2519.047377] QJump[4]: Jump[4]: Delaying 279940 cycles per network tick (99us)  
[ 2519.047382] QJump[4]: Queue 0 = @ 10Mb/s   
[ 2519.047383] qjump[4]: Init fifo limit=128  
[ 2519.047384] Bands= 1  

To remove as a TC module:
sudo tc qdisc del dev eth0 root qjump



