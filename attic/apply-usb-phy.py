#!/usr/bin/env python3
"""Add Apple S5L USB PHY driver to kernel tree."""
import os, shutil, sys

tree = sys.argv[1]

# 1. Копируем драйвер
src = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'drivers', 'phy-apple-s5l-usb.c')
dst = os.path.join(tree, 'drivers/phy/phy-apple-s5l-usb.c')
shutil.copy(src, dst)
print(f'  copied phy-apple-s5l-usb.c')

# 2. Добавить в drivers/phy/Kconfig
kconfig_path = os.path.join(tree, 'drivers/phy/Kconfig')
with open(kconfig_path) as f:
    text = f.read()

entry = """
config PHY_APPLE_S5L_USB
\ttristate "Apple S5L USB PHY"
\tdepends on ARCH_APPLE_S5L
\tselect GENERIC_PHY
\thelp
\t  USB PHY driver for Apple S5L SoCs (A4/A5).
"""

if 'PHY_APPLE_S5L_USB' not in text:
    text = text.rstrip() + '\n' + entry + '\n'
    with open(kconfig_path, 'w') as f:
        f.write(text)
    print('  patched drivers/phy/Kconfig')

# 3. Добавить в drivers/phy/Makefile
makefile_path = os.path.join(tree, 'drivers/phy/Makefile')
with open(makefile_path) as f:
    text = f.read()

line = 'obj-$(CONFIG_PHY_APPLE_S5L_USB)\t+= phy-apple-s5l-usb.o\n'
if 'phy-apple-s5l-usb' not in text:
    text = text.rstrip() + '\n' + line
    with open(makefile_path, 'w') as f:
        f.write(text)
    print('  patched drivers/phy/Makefile')

print('Done')
