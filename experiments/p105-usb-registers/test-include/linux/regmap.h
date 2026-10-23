#ifndef TEST_REGMAP_H
#define TEST_REGMAP_H
struct regmap { unsigned int id, values[2048]; };
int regmap_read(struct regmap *m,unsigned int off,unsigned int *value);
int regmap_write(struct regmap *m,unsigned int off,unsigned int value);
#endif
