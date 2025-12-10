#!/bin/bash
sudo chgrp dmi /sys/firmware/dmi/tables/DMI
sudo chgrp dmi /sys/firmware/dmi/tables/smbios_entry_point
sudo chmod 440 /sys/firmware/dmi/tables/DMI
sudo chmod 440 /sys/firmware/dmi/tables/smbios_entry_point
ls -la /sys/firmware/dmi/tables/
