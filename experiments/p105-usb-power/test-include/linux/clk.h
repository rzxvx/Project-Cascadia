#ifndef FIXTURE_CLK_H
#define FIXTURE_CLK_H
struct clk { unsigned long hz; };
unsigned long clk_get_rate(struct clk *c);
#endif
