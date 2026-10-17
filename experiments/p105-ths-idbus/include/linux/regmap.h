#ifndef FAKE_REGMAP_H
#define FAKE_REGMAP_H
#include <linux/types.h>
struct regmap { void *cookie; int val_bytes; };
static inline int regmap_get_val_bytes(struct regmap *m) { return m->val_bytes; }
int regmap_write(struct regmap *, unsigned int, unsigned int);
int regmap_read(struct regmap *, unsigned int, unsigned int *);
int regmap_bulk_write(struct regmap *, unsigned int, const void *, size_t);
int regmap_bulk_read(struct regmap *, unsigned int, void *, size_t);
#endif
