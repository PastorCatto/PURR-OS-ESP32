# user_drivers/

Drop custom or community-made drivers here. `modulestrap` scans this folder automatically alongside `source/drivers/`.

## Layout

Two layouts are supported:

**Flat** (single driver at root):
```
user_drivers/
  my_display/
    driver.pcat
    my_display.c
    my_display.h
```

**Typed** (grouped by type, same as source/drivers/):
```
user_drivers/
  display/
    my_display/
      driver.pcat
      my_display.c
```

## driver.pcat minimum

```toml
name            = "my_display"
version         = "0.1.0"
type            = "display"
kernel_min      = "0.9.0"
kernel_max      = ""
provides        = ["catcall_display"]
required_catcalls = []
```

## Extra paths

You can also point modulestrap at any folder outside this repo:

```
modulestrap build all --drivers /path/to/external_drivers
```
