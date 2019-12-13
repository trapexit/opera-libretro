#pragma once

const char *freedo_debug_swi_func(uint32_t swi);
int freedo_debug_arm_disassemble(const uint32_t pc,
                                 const uint32_t op);
