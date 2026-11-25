#include "../firmware/anchor/src/uart_maintenance.h"
#include <assert.h>
int main() {
 UartMaintenanceGate g;
 assert(!g.take(0,true)); // bare DOWNLOAD is never authorized
 assert(!g.arm(0,false)); // streaming / incomplete initialization
 assert(g.arm(10,true));
 assert(!g.take(11,false)); // phase changed after authorization
 assert(!g.take(11,true)); // one shot, even on rejection
 assert(g.arm(12,true));assert(g.take(13,true));assert(!g.take(14,true));
 assert(g.arm(0,true));assert(!g.take(120000,true));
 assert(g.arm(0xfffffff0u,true));assert(g.take(5,true)); // tick wrap
 assert(g.arm(0,true));g.reset();assert(!g.take(1,true)); // PH11 reset
}
