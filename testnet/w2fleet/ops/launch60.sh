#!/bin/bash
cd /mnt/pve/Node14TB/hemis-ptx/src/hemisd/testnet/w2fleet
exec python3 -u run_bootstrap.py --n 60 --reg-out /mnt/pve/Node14TB/hemis-ptx/w2-fleet/registration-N60.json
