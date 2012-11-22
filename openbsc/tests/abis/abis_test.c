/*
 * (C) 2012 by Holger Hans Peter Freyther <zecke@selfish.org>
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include <osmocom/core/application.h>
#include <osmocom/core/utils.h>

#include <openbsc/gsm_data.h>
#include <openbsc/abis_nm.h>
#include <openbsc/debug.h>

static const uint8_t simple_config[] = {
	/*0, 13, */
	66, 18, 0, 3, 1, 2, 3, 19, 0, 3, 3, 4, 5,
};

static const uint8_t dual_config[] = {
	/*0, 26, */
	66, 18, 0, 3, 1, 2, 3, 19, 0, 3, 3, 4, 5,
	66, 18, 0, 3, 9, 7, 5, 19, 0, 3, 6, 7, 8,
};

static void test_simple_sw_config(void)
{
	struct abis_nm_sw_descr descr[1];
	int rc;

	rc = abis_nm_parse_sw_config(simple_config, ARRAY_SIZE(simple_config),
				&descr[0], ARRAY_SIZE(descr));
	if (rc != 1) {
		printf("FAILED to parse the File Id/File version\n");
		abort();
	}

	if (descr[0].len != 13) {
		printf("WRONG SIZE: %d\n", descr[0].len);
		abort();
	}

	printf("Start: %u len: %zu\n", descr[0].start - simple_config, descr[0].len);
	printf("file_id:  %s\n", osmo_hexdump(descr[0].file_id, descr[0].file_id_len));
	printf("file_ver: %s\n", osmo_hexdump(descr[0].file_ver, descr[0].file_ver_len));
}

static void test_simple_sw_short(void)
{
	struct abis_nm_sw_descr descr[1];
	int i;

	for (i = 1; i < ARRAY_SIZE(simple_config); ++i) {
		int rc = abis_nm_parse_sw_config(simple_config,
				ARRAY_SIZE(simple_config) - i, &descr[0],
				ARRAY_SIZE(descr));
		if (rc >= 1) {
			printf("SHOULD not have parsed: %d\n", rc);
			abort();
		}
	}
}

static void test_dual_sw_config(void)
{
	struct abis_nm_sw_descr descr[2];
	int rc;

	rc = abis_nm_parse_sw_config(dual_config, ARRAY_SIZE(dual_config),
				&descr[0], ARRAY_SIZE(descr));
	if (rc != 2) {
		printf("FAILED to parse the File Id/File version\n");
		abort();
	}

	if (descr[0].len != 13) {
		printf("WRONG SIZE0: %d\n", descr[0].len);
		abort();
	}

	if (descr[1].len != 13) {
		printf("WRONG SIZE1: %d\n", descr[1].len);
		abort();
	}

	printf("Start: %u len: %zu\n", descr[0].start - dual_config, descr[0].len);
	printf("file_id:  %s\n", osmo_hexdump(descr[0].file_id, descr[0].file_id_len));
	printf("file_ver: %s\n", osmo_hexdump(descr[0].file_ver, descr[0].file_ver_len));

	printf("Start: %u len: %zu\n", descr[1].start - dual_config, descr[1].len);
	printf("file_id:  %s\n", osmo_hexdump(descr[1].file_id, descr[1].file_id_len));
	printf("file_ver: %s\n", osmo_hexdump(descr[1].file_ver, descr[1].file_ver_len));
}

int main(int argc, char **argv)
{
	osmo_init_logging(&log_info);
	test_simple_sw_config();
	test_simple_sw_short();
	test_dual_sw_config();

	return EXIT_SUCCESS;
}
