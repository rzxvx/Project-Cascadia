/* SPDX-License-Identifier: GPL-2.0 */
/* Whole actual transport/parser sources; synthetic regmap and test lease only.
 * stdin/stdout carry synthetic PUBLIC signatures, never private key material.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/errno.h>
#include "cascadia-ths-idbus.h"
#include "cascadia-authcp-aid.h"
struct wire_frame { u8 tx[32], rx[32]; size_t tx_len, rx_len; };
struct fake_bus {
	struct mutex lock;
	struct regmap map;
	struct cascadia_ths_idbus transport;
	u8 reply[32];
	size_t reply_len;
};
static int lease(void *cookie, u64 owner, u64 epoch)
{
	struct fake_bus *b = cookie;
	assert(b->lock.locked);
	return owner == b->transport.owner && epoch == 19 ? 0 : -ESTALE;
}
int regmap_bulk_write(struct regmap *m, unsigned reg, const void *buf, size_t n)
{
	struct fake_bus *b = m->cookie;
	assert(b->lock.locked && reg == 0x20 && n >= 1 && n <= 32 && buf);
	return 0;
}
int regmap_write(struct regmap *m, unsigned reg, unsigned value)
{
	struct fake_bus *b = m->cookie;
	assert(b->lock.locked && (reg == 0x40 || reg == 0x41) && value <= 63);
	return 0;
}
int regmap_read(struct regmap *m, unsigned reg, unsigned *v)
{
	struct fake_bus *b = m->cookie;
	assert(b->lock.locked && reg == 0x0d);
	*v = 0x10;
	return 0;
}
int regmap_bulk_read(struct regmap *m, unsigned reg, void *buf, size_t n)
{
	struct fake_bus *b = m->cookie;
	assert(b->lock.locked && reg == 0x40 - n && n == b->reply_len);
	for (size_t i = 0; i < n; i++)
		((u8 *)buf)[i] = b->reply[n - i - 1];
	return 0;
}
#include "frames.inc"
static void run(const struct wire_frame *frames, size_t count, u8 version,
		bool late, const u8 *input_signature)
{
	struct fake_bus b = {0};
	struct cascadia_aid_exchange a = {0};
	struct cascadia_aid_request q;
	struct cascadia_ths_lease l = {lease, &b};
	u8 nonce[32], reply[32], signature[128], old[32];
	size_t n, len;
	u64 id, last = 0;
	bool have_old = false, substituted = false;

	assert(!pthread_mutex_init(&b.lock.value, NULL));
	b.map.cookie = &b;
	b.map.val_bytes = 1;
	assert(!cascadia_ths_init(&b.transport, &b.map, &b.lock, &l, 19));
	for (size_t i = 0; i < 32; i++)
		nonce[i] = i;
	assert(!cascadia_aid_init(&a, 19, true, version, 32, 32, nonce,
				 version == 3 ? 32 : 20));
	for (size_t i = 0; i < count; i++) {
		assert(!cascadia_aid_plan(&a, 19, &q));
		assert(q.tx_len == frames[i].tx_len && q.rx_len == frames[i].rx_len);
		assert(!memcmp(q.tx, frames[i].tx, q.tx_len));
		assert(!cascadia_ths_submit(&b.transport, b.transport.owner, 19,
					 q.tx, q.tx_len, q.rx_len, &id));
		assert(id > last);
		last = id;
		b.reply_len = frames[i].rx_len;
		memcpy(b.reply, frames[i].rx, b.reply_len);
		if (q.tx[0] == 0x98) {
			size_t offset = ((size_t)q.tx[1] << 8) | q.tx[2];
			assert(offset + q.rx_len - 1 <= (version == 3 ? 64U : 128U));
			memcpy(b.reply + 1, input_signature + offset, q.rx_len - 1);
		}
		if (late && q.tx[0] == 0x98 && q.rx_len == 32) {
			if (have_old && !substituted) {
				memcpy(b.reply, old, 32);
				substituted = true;
			} else if (!have_old) {
				memcpy(old, b.reply, 32);
				have_old = true;
			}
		}
		assert(!cascadia_ths_receive(&b.transport, b.transport.owner, 19,
					   id, reply, 32, &n));
		assert(!cascadia_aid_complete(&a, 19, 0, reply, n));
	}
	assert(!cascadia_aid_copy_signature(&a, 19, signature, 128, &len));
	assert(len == (version == 3 ? 64U : 128U));
	if (late) {
		assert(substituted);
		assert(!memcmp(signature, signature + 31, 31));
	} else {
		assert(!memcmp(signature, input_signature, len));
	}
	assert(fwrite(signature, 1, len, stdout) == len);
	assert(!pthread_mutex_destroy(&b.lock.value));
}
int main(int argc, char **argv)
{
	u8 signature[128];
	unsigned version;
	bool late;
	size_t length;
	if (argc != 3 || (strcmp(argv[1], "2") && strcmp(argv[1], "3")) ||
	    (strcmp(argv[2], "0") && strcmp(argv[2], "1")))
		return 2;
	version = !strcmp(argv[1], "2") ? 2 : 3;
	late = !strcmp(argv[2], "1");
	length = version == 3 ? 64 : 128;
	if (fread(signature, 1, length, stdin) != length || fgetc(stdin) != EOF)
		return 2;
	if (version == 2)
		run(v2, sizeof(v2) / sizeof(v2[0]), 2, late, signature);
	else
		run(v3, sizeof(v3) / sizeof(v3[0]), 3, late, signature);
	return 0;
}
