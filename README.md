# phyphoxBLE-MBED-OS

main.cpp Template:

```c++
#include "mbed.h"
#include "phyphoxBle.h"
#include "power_save.h"
int main()
{
    power_save();

    PhyphoxBLE::start("test");
    while (true) {
        ThisThread::sleep_for(100ms);

    }
}
```

