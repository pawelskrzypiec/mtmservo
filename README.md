# mtmservo
*mtmservo* is the simple Raspberry Pi 3 platform driver for the stepper motor control.

Its based on the project realized during embedded systems classes in Microelectronics in Industry
and Medicine at AGH UST.


**User space interface**

The driver exports to the Raspberry Pi 3 userspace the following files:
* _calibration_ (RW): starts the calibration procedure (W); returns the state of calibration
    procedure (R),
* _detector_ (R): returns the detector state (R),
* _dst_position_ (RW): sets the destined position and starts motor stepping (W); returns the last
    written value (R),
* _frequency_ (RW): sets the stepping frequency (W); returns the actual stepping frequency (R),
* _position_ (R): returns the actual stepper motor position (R).

```bash
root@raspberrypi:~ # ls /sys/bus/platform/devices/mtmservo/
calibration  detector  driver  driver_override  dst_position  frequency  modalias  of_node  position  power  subsystem  uevent
```
