#!/usr/bin/env python3
"""Exercise actual PMU GPIO callbacks against a fake regmap, without hardware."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'patches/files-sandcastle-port/drivers/mfd/apple-pmu-i2c.c'


def function(text, name):
    start = text.index('static int ' + name + '(')
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class PmuGpio(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'requires a local C compiler')
    def test_real_callbacks_propagate_transfer_errors_and_preserve_direction(self):
        text = SOURCE.read_text()
        callbacks = '\n'.join(function(text, name) for name in (
            'apple_pmu_gpio_cfg', 'apple_pmu_gpio_get_direction',
            'apple_pmu_gpio_get', 'apple_pmu_gpio_direction_output',
            'apple_pmu_gpio_direction_input'))
        definitions = '\n'.join(line for line in text.splitlines()
                                if line.startswith(('#define D1946_GPIO_CFG ',
                                                    '#define D1946_GPIO_LEVEL ',
                                                    '#define D1946_GPIO_IS_INPUT(')))
        self.assertEqual(len(definitions.splitlines()), 3)
        harness = r'''
#include <assert.h>
#include <errno.h>
struct regmap { int unused; };
struct apple_pmu_i2c { struct regmap *regmap; };
struct gpio_chip { struct apple_pmu_i2c *data; };
#define GPIO_LINE_DIRECTION_IN 1
#define GPIO_LINE_DIRECTION_OUT 0
static unsigned cfg_value, written_reg, written_mask, written_value;
static int read_error, write_error, reads, writes;
static struct apple_pmu_i2c *gpiochip_get_data(struct gpio_chip *gc)
{ return gc->data; }
static int regmap_read(struct regmap *map, unsigned reg, unsigned *value)
{ (void)map; assert(reg == D1946_GPIO_CFG + 3); reads++;
  if (read_error) return read_error;
  *value = cfg_value; return 0; }
static int regmap_update_bits(struct regmap *map, unsigned reg,
                              unsigned mask, unsigned value)
{ (void)map; writes++; written_reg=reg; written_mask=mask;
  written_value=value; return write_error; }
'''.replace('static unsigned cfg_value', definitions + '\nstatic unsigned cfg_value') + callbacks + r'''
int main(void) {
  struct regmap map = {0};
  struct apple_pmu_i2c pmu = {&map};
  struct gpio_chip gc = {&pmu};
  cfg_value=0x09;
  assert(apple_pmu_gpio_direction_output(&gc,3,1)==0);
  assert(reads==1 && writes==1 && written_reg==0x64);
  assert(written_mask==2 && written_value==2);
  write_error=-EIO;
  assert(apple_pmu_gpio_direction_output(&gc,3,0)==-EIO);
  assert(written_value==0 && writes==2);
  read_error=-ETIMEDOUT;
  assert(apple_pmu_gpio_direction_output(&gc,3,1)==-ETIMEDOUT);
  assert(apple_pmu_gpio_direction_input(&gc,3)==-ETIMEDOUT);
  assert(apple_pmu_gpio_get(&gc,3)==-ETIMEDOUT && writes==2);
  read_error=0;
  cfg_value=0x60;
  assert(apple_pmu_gpio_direction_output(&gc,3,1)==-EPERM && writes==2);
  assert(apple_pmu_gpio_direction_input(&gc,3)==0);
  cfg_value=0x09;
  assert(apple_pmu_gpio_direction_input(&gc,3)==-EPERM);
  assert(apple_pmu_gpio_get(&gc,3)==0);
  cfg_value=0x0b;
  assert(apple_pmu_gpio_get(&gc,3)==1);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'pmu.c'
            binary = Path(temp) / 'pmu-test'
            source.write_text(harness)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(source), '-o', str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
