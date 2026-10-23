// SPDX-License-Identifier: GPL-2.0
// Register map access API; Copyright 2011 Wolfson Microelectronics plc.
// Author Mark Brown; extracted unmodified functions, Linux6.12 adc218676eef25575469234709c2d87185ca223a.
int regmap_read(struct regmap *map, unsigned int reg, unsigned int *val)
{
	int ret;

	if (!IS_ALIGNED(reg, map->reg_stride))
		return -EINVAL;

	map->lock(map->lock_arg);

	ret = _regmap_read(map, reg, val);

	map->unlock(map->lock_arg);

	return ret;
}
int regmap_write(struct regmap *map, unsigned int reg, unsigned int val)
{
	int ret;

	if (!IS_ALIGNED(reg, map->reg_stride))
		return -EINVAL;

	map->lock(map->lock_arg);

	ret = _regmap_write(map, reg, val);

	map->unlock(map->lock_arg);

	return ret;
}
static int _regmap_update_bits(struct regmap *map, unsigned int reg,
			       unsigned int mask, unsigned int val,
			       bool *change, bool force_write)
{
	int ret;
	unsigned int tmp, orig;

	if (change)
		*change = false;

	if (regmap_volatile(map, reg) && map->reg_update_bits) {
		reg = regmap_reg_addr(map, reg);
		ret = map->reg_update_bits(map->bus_context, reg, mask, val);
		if (ret == 0 && change)
			*change = true;
	} else {
		ret = _regmap_read(map, reg, &orig);
		if (ret != 0)
			return ret;

		tmp = orig & ~mask;
		tmp |= val & mask;

		if (force_write || (tmp != orig) || map->force_write_field) {
			ret = _regmap_write(map, reg, tmp);
			if (ret == 0 && change)
				*change = true;
		}
	}

	return ret;
}
int regmap_update_bits_base(struct regmap *map, unsigned int reg,
			    unsigned int mask, unsigned int val,
			    bool *change, bool async, bool force)
{
	int ret;

	map->lock(map->lock_arg);

	map->async = async;

	ret = _regmap_update_bits(map, reg, mask, val, change, force);

	map->async = false;

	map->unlock(map->lock_arg);

	return ret;
}
