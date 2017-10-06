# Dom-0 settings:
```Linux nimbnode44 4.10.0-35-generic #39-Ubuntu SMP Wed Sep 13 07:46:59 UTC 2017 x86_64 x86_64 x86_64 GNU/Linux```

# Dom-U settings:
```Linux ubuntu-hvm 4.10.0-20-generic #22-Ubuntu SMP Thu Apr 20 09:22:42 UTC 2017 x86_64 x86_64 x86_64 GNU/Linux```

# Setting up CRIMES with Xen-4.9:

Just run
```sh -c "`curl -fsSL https://gist.githubusercontent.com/SunnyRaj/2718865397d09eea152714dacda03bc9/raw/45ec24e226bf7875aa48b2ac0e032ada35c4cfc9/crimes-install.sh`"```

This will

* Install Xen
* Install libVMI
* Install memevents code

Once these are install reboot the machine and create a ubuntu-hvm
* Setup libVMI for the VM by creating appropriate `libvmi.conf` file.
* Ensure `sudo vmi-process-list ubuntu-hvm` works.
* Copy `crimes-4.9/CRIMES/guest-malloc-wrapper/` folder into the VM
* Inside the VM, run `cd guest-malloc-wrapper && ./compile.sh && ./test-single-memevent`
* In Dom-0 run `cd ~/crimes-4.9/CRIMES && ./build_modules.sh`
* Once this is built run `sudo ./mem-single-page-event ubuntu-hvm <pid> <address>`
* The `pid` and `address` are the outputs of the `test-single-memevent` from the VM.
