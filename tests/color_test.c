#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "color.h"

#include "check.h"

/* Round-trip goes through 0..255 ints both ways, so a parsed float can land
 * up to 1/255 (~0.004) off. 0.005 is that with margin. */
static int near(float a, float b) {
	float d = a - b;
	return (d < 0.005f) && (d > -0.005f);
}

int main(void) {
	float r, g, b, a;
	float r2, g2, b2, a2;
	char buf[9];

	CHECK(color_parse_hex("ff0000ff", &r, &g, &b, &a) == 1, "parse accepts canonical 8-hex RRGGBBAA");
	CHECK(near(r, 1.0f) && near(g, 0.0f) && near(b, 0.0f) && near(a, 1.0f), "8-hex maps each byte to its own channel");

	CHECK(color_parse_hex("#0000ff00", &r, &g, &b, &a) == 1, "parse tolerates a leading hash");
	CHECK(near(r, 0.0f) && near(g, 0.0f) && near(b, 1.0f) && near(a, 0.0f), "leading hash does not shift the channels");

	CHECK(color_parse_hex("00ff00", &r, &g, &b, &a) == 1, "parse accepts 6-hex RRGGBB");
	CHECK(near(r, 0.0f) && near(g, 1.0f) && near(b, 0.0f) && near(a, 1.0f), "6-hex defaults alpha to fully opaque");

	CHECK(color_parse_hex("#112233", &r, &g, &b, &a) == 1, "parse accepts 6-hex with a leading hash");
	CHECK(near(r, 0x11 / 255.0f), "red byte scales to its float");
	CHECK(near(g, 0x22 / 255.0f), "green byte scales to its float");
	CHECK(near(b, 0x33 / 255.0f), "blue byte scales to its float");
	CHECK(near(a, 1.0f), "6-hex still defaults alpha to fully opaque");

	CHECK(color_parse_hex("ABCDEF12", &r,  &g,  &b,  &a)  == 1, "parse accepts upper-case hex");
	CHECK(color_parse_hex("abcdef12", &r2, &g2, &b2, &a2) == 1, "parse accepts lower-case hex");
	CHECK(near(r, r2) && near(g, g2) && near(b, b2) && near(a, a2), "parse is case-insensitive, so both spellings agree");

	CHECK(color_parse_hex(NULL,        &r, &g, &b, &a) == 0, "parse rejects a NULL string");
	CHECK(color_parse_hex("",          &r, &g, &b, &a) == 0, "parse rejects an empty string");
	CHECK(color_parse_hex("zzzzzz",    &r, &g, &b, &a) == 0, "parse rejects non-hex characters");
	CHECK(color_parse_hex("12345",     &r, &g, &b, &a) == 0, "parse rejects 5 digits, one short of 6-hex");
	CHECK(color_parse_hex("1234567",   &r, &g, &b, &a) == 0, "parse rejects 7 digits, between the two legal lengths");
	CHECK(color_parse_hex("123456789", &r, &g, &b, &a) == 0, "parse rejects 9 digits, one past 8-hex");
	CHECK(color_parse_hex("12345g",    &r, &g, &b, &a) == 0, "parse rejects a bad nibble at the right length");

	color_to_hex(0.0f, 0.0f, 0.0f, 0.0f, buf);
	CHECK(strcmp(buf, "00000000") == 0, "format writes all-zero for the low bound");
	color_to_hex(1.0f, 1.0f, 1.0f, 1.0f, buf);
	CHECK(strcmp(buf, "ffffffff") == 0, "format writes all-f for the high bound");

	color_to_hex(-0.5f, 2.0f, 0.5f, 1.0f, buf);
	CHECK(buf[0] == '0' && buf[1] == '0', "format clamps a negative channel to 00");
	CHECK(buf[2] == 'f' && buf[3] == 'f', "format clamps an above-one channel to ff");
	CHECK(buf[4] == '8' && buf[5] == '0', "format rounds a half-way channel to 80");
	CHECK(buf[6] == 'f' && buf[7] == 'f', "format leaves an in-range channel alone");

	memset(buf, 'X', sizeof(buf));
	color_to_hex(0.0f, 0.0f, 0.0f, 0.0f, buf);
	CHECK(buf[8] == '\0', "format always terminates, so the caller can print it");

	{
		float r0 = 0.25f, g0 = 0.5f, b0 = 0.75f, a0 = 1.0f;
		color_to_hex(r0, g0, b0, a0, buf);
		CHECK(color_parse_hex(buf, &r, &g, &b, &a) == 1, "parse accepts what format wrote");
		CHECK(near(r, r0) && near(g, g0) && near(b, b0) && near(a, a0), "format then parse returns the same color");
	}

	{
		float r0 = 0x12 / 255.0f, g0 = 0x34 / 255.0f;
		float b0 = 0x56 / 255.0f, a0 = 0x78 / 255.0f;
		color_to_hex(r0, g0, b0, a0, buf);
		CHECK(strcmp(buf, "12345678") == 0, "format keeps four distinct channels in order");
		CHECK(color_parse_hex(buf, &r, &g, &b, &a) == 1, "parse accepts four distinct channels");
		CHECK(near(r, r0) && near(g, g0) && near(b, b0) && near(a, a0), "round-trip survives four distinct channels");
	}

	CHECK_PASS("test-color");
}
