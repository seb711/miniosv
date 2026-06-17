/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef DRIVERS_CONSOLE_DRIVER_HH
#define DRIVERS_CONSOLE_DRIVER_HH

#include <cstddef>

namespace console {

// The console is output-only: there is no /dev/console and nothing reads
// console input, so a driver only needs to emit bytes. start() performs any
// device initialization via dev_start().
class console_driver {
public:
    virtual ~console_driver() {}
    virtual void write(const char *str, size_t len) = 0;
    virtual void flush() = 0;
    void start();
private:
    virtual void dev_start() = 0;
};

};

#endif
