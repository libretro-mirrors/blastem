#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "vgm.h"

vgm_writer *vgm_write_open(char *filename, uint32_t rate, uint32_t clock, uint32_t cycle)
{
	FILE *f = fopen(filename, "wb");
	if (!f) {
		return NULL;
	}
	vgm_writer *writer = calloc(sizeof(vgm_writer), 1);
	memcpy(writer->header.ident, "Vgm ", 4);
	writer->header.version = 0x150;
	writer->header.data_offset = sizeof(writer->header) - offsetof(vgm_header, data_offset);
	writer->header.rate = rate;
	writer->f = f;
	if (1 != fwrite(&writer->header, sizeof(writer->header), 1, f)) {
		free(writer);
		fclose(f);
		return NULL;
	}
	writer->master_clock = clock;
	writer->last_cycle = cycle;
	
	return writer;
}

void vgm_sn76489_init(vgm_writer *writer, uint32_t clock, uint16_t feedback, uint8_t shift_reg_size, uint8_t flags)
{
	if (flags && writer->header.version < 0x151) {
		writer->header.version = 0x151;
	}
	writer->header.sn76489_clk = clock,
	writer->header.sn76489_fb = feedback;
	writer->header.sn76489_shift = shift_reg_size;
	writer->header.sn76489_flags = flags;
}

static void wait_commands(vgm_writer *writer, uint32_t delta)
{
	if (!delta) {
		return;
	}
	if (delta <= 0x10) {
		fputc(CMD_WAIT_SHORT + (delta - 1), writer->f);
	} else if (delta >= 735 && delta <= (735 + 0x10)) {
		fputc(CMD_WAIT_60, writer->f);
		wait_commands(writer, delta - 735);
	} else if (delta >= 882 && delta <= (882 + 0x10)) {
		fputc(CMD_WAIT_50, writer->f);
		wait_commands(writer, delta - 882);
	} else if (delta > 0xFFFF) {
		uint8_t cmd[3] = {CMD_WAIT, 0xFF, 0xFF};
		fwrite(cmd, 1, sizeof(cmd), writer->f);
		wait_commands(writer, delta - 0xFFFF);
	} else {
		uint8_t cmd[3] = {CMD_WAIT, delta, delta >> 8};
		fwrite(cmd, 1, sizeof(cmd), writer->f);
	}
}

static void add_wait(vgm_writer *writer, uint32_t cycle)
{
	uint64_t delta = cycle - writer->last_cycle;
	delta *= (uint64_t)44100;
	delta /= (uint64_t)writer->master_clock;
	
	uint32_t mclks_per_sample = writer->master_clock / 44100;
	writer->last_cycle += delta * mclks_per_sample; 
	writer->header.num_samples += delta;
	wait_commands(writer, delta);
}

void vgm_sn76489_write(vgm_writer *writer, uint32_t cycle, uint8_t value)
{
	add_wait(writer, cycle);
	uint8_t cmd[2] = {CMD_PSG, value};
	fwrite(cmd, 1, sizeof(cmd), writer->f);
}

void vgm_ym2612_init(vgm_writer *writer, uint32_t clock)
{
	writer->header.ym2612_clk = clock;
}

void vgm_ym2612_part1_write(vgm_writer *writer, uint32_t cycle, uint8_t reg, uint8_t value)
{
	add_wait(writer, cycle);
	uint8_t cmd[3] = {CMD_YM2612_0, reg, value};
	fwrite(cmd, 1, sizeof(cmd), writer->f);
}

void vgm_ym2612_part2_write(vgm_writer *writer, uint32_t cycle, uint8_t reg, uint8_t value)
{
	add_wait(writer, cycle);
	uint8_t cmd[3] = {CMD_YM2612_1, reg, value};
	fwrite(cmd, 1, sizeof(cmd), writer->f);
}

void vgm_adjust_cycles(vgm_writer *writer, uint32_t deduction)
{
	if (deduction > writer->last_cycle) {
		writer->last_cycle = 0;
	} else {
		writer->last_cycle -= deduction;
	}
}

void vgm_close(vgm_writer *writer)
{
	writer->header.eof_offset = ftell(writer->f) - offsetof(vgm_header, eof_offset);
	fseek(writer->f, SEEK_SET, 0);
	fwrite(&writer->header, sizeof(writer->header), 1, writer->f);
	fclose(writer->f);
	free(writer);
}