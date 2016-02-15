Simple X11 input driver for Apple MacBook Pro touchpad
======================================================

How to install
--------------

    ./autogen.sh
    ./configure
    make
    sudo make install
    sudo cp 50-random.conf /usr/share/X11/xorg.conf.d/

Currently it installs the driver into `/usr/local/lib/xorg/modules/input/` but X11 searches for drivers in `/usr/share/X11/xorg.conf/.d/`. So move the driver files over:

    sudo mv /usr/local/lib/xorg/modules/input/random_drv.so /usr/lib/xorg/modules/input/
    sudo mv /usr/local/lib/xorg/modules/input/random_drv.la /usr/lib/xorg/modules/input/

 or create symlinks as I did, so that you can edit the source code and `make install` any time in the future:

    sudo ln -s /usr/local/lib/xorg/modules/input/random_drv.so /usr/lib/xorg/modules/input/
    sudo ln -s /usr/local/lib/xorg/modules/input/random_drv.la /usr/lib/xorg/modules/input/

What it supports
----------------

The following features are supported (but non configurable after compilation):

-   Move cursor with one finger.
-   Click with hardware button emulates mouse left click.
-   2 finger hardware button click to emulate right click (so hold 2 fingers on the trackpad and push down both fingers to click the hardware button).
-   Mouse acceleration (if you move your finger faster on the trackpad, it will move the cursor exponentially faster on the screen).
-   Tap-to-click.
-   Scroll with 2 fingers with momentum. This is a remake of the scrolling of the Mac OS X, but much worse since on linux scrolling is emulated with button clicks (so you cannot scroll slowly pixel-by-pixel as on OS X).
-   Drag with 3 fingers (can continue the 3 finger drag if put back the 3 fingers within a certain amount of time).

Not supported:

-   2 finger tap to emulate right click.
-   Hold down one finger using the hardware button and drag using an other finger.
-   Features like scrolling with one finger on the right, or circular scrolling, etc because I never used those.

Why not use Synaptics?
----------------------

I have bad experiences with Synaptics driver, namely it generated fake `left mouse button down` events when I put my 2 fingers on the trackpad at not exactly the same time and started scrolling. Which meant it was clicking around in the source code when I only tried to scroll, or clicked on random links in the browser during scrolling. And sometimes started selecting the text. I tried tweaking the settings, but couldn't solve these issues.

I also looked at the source code of synaptics: it is huge, several thousands of lines of code, poorly documented what is happening and why. Then I thought: "I can easily write a better driver than this".

So I wrote this driver, which became huge, more than a thousand lines of code and is poorly documented what is happening and why... But at least it supports dragging with 3 fingers and does not generate fake mouse click events during scrolling.

License
-------

GPL

This driver is a modified version of the "random" input driver written by Peter Hutterer found on [this page](http://www.x.org/wiki/Development/Documentation/XorgInputHOWTO/). He says that "Warning: This tutorial is outdated and refers to APIs that have since been changed", but it worked for me.

What I used
-----------

-   Linux Mint 17.3 Cinnamon
-   Kernel: 3.19.0-32-generic #37~14.04.1-Ubuntu SMP Thu Oct 22 09:41:40 UTC 2015 x86_64 x86_64 x86_64 GNU/Linux
-   xorg 1:7.7+1ubuntu8.1
-   cinnamon 2.8.6+rosa
-   Macbook Pro early 2011
