# Adelie: Continuous Address Space Layout Re-randomization for Linux Drivers

* Publications

    Adelie: Continuous Address Space Layout Re-randomization for Linux Drivers.
	In the Proceedings of the 27th ACM International Conference on
	Architectural Support for Programming Languages and Operating Systems
	(ASPLOS'22). Lausanne, Switzerland

    [Paper](https://rusnikola.github.io/files/adelie-asplos22.pdf)

	[Artifact Package](https://zenodo.org/record/5831326)

* Source code license

	See LICENSE for more details

## Linux kernel module re-randomization

Install packages:
sudo apt-get update
sudo apt-get upgrade
sudo apt-get install libssl-dev libelf-dev bison flex build-essential
sudo apt-get install gcc-8 g++-8

Run gcc\_install.sh to set up gcc-8 as the default compiler

To enable the Linux kernel re-randomization module, do the following:

1) Get the 5.0.4 version of the kernel: https://www.kernel.org/pub/linux/kernel/v5.x/linux-5.0.4.tar.xz

2) Unpack linux-5.0.4.tar.xz

3) cd to linux repo and apply these patches to kernel:

> ```bash
> > patch -p1 < ../pie-v6.patch
> > patch -p1 < ../pic-support-v6.patch
> > patch -p1 < ../kaslr_basic.patch
> ```

Also manual patches (or use gcc plugins, see below):

> ```bash
> > patch -p1 < ../manual/kaslr_e1000.patch
> > patch -p1 < ../manual/kaslr_e1000e.patch
> > patch -p1 < ../manual/kaslr_nvme.patch
> > patch -p1 < ../manual/kaslr_fuse.patch
> ```

4) Copy .config from virtue to kernel

> ```bash
> > cp ../config-full-kaslr .config
> ```

5) Make kernel and install

> ```bash
> > make -j8
> 
> > sudo make modules_install install
> ```

6) Reboot and load in randmod with the desired module(s) and randomization period (20 is default)

> ```bash
> sudo modprobe randmod module_names=e1000 rand_period=20
> ```

*Note: e1000 or other modules loaded in must have re-randomization changes applied. More on this to follow....*

Modules available for re-randomization: e1000, e1000e, fuse, xhci, ext4, nvme

### Using plugins

Rather than applying patch -p1 < ../kaslr_e1000.patch (or other driver-specific patches) in step 3 above, modules can be compiled with plugin(s) for re-randomization.
To do this, cd to gcc-plugins directory and run make. 

*Note: This has been tested with gcc/g++ 8, so that is recommended. Also, gcc-8-plugin-dev package should be installed.*

Then, go to the Linux repo and in the Makefile for the module to be rerandomized, and add:

```bash
EXTRA_CFLAGS += -fplugin=/path/to/fix_relocations_plugin.so
```
For example, for the e1000e driver, this should be added to drivers/net/ethernet/intel/e1000e/Makefile.

Multiple fplugin arguments can be used in order to apply multiple plugins (string, propepilogue, function wrapper). 

You will also need to flag the module as rerandomizable. To do this, go to the main .c file for the module and add:

```bash
MODULE_INFO(randomizable, "Y");
```
For example, for the e1000 driver, this should be added to drivers/net/ethernet/intel/e1000/e1000_main.c.

From there, the kernel can be compiled and the plugin(s) will be used on the specified modules.

##### About each plugin

fix_relocations_plugin.so   // String plugin

function_proepilogue_plugin.c   // Adds function prologues and epilogues

rerandomization_wrapper_plugin.c   // Wraps functions for re-randomization
