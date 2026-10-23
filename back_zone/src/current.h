#pragma once

enum { CUR_LEFT, CUR_RIGHT, CUR_SERVO, CUR_COUNT };

int current_init(void);
void current_read_ma(int ma[CUR_COUNT]);