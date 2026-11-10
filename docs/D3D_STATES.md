# Состояния D3D в BB: таблицы сброса устройства

Сгенерировано: `python -I tools/d3d_map.py <лог --d3d_census> states`. Источник — две
таблицы, которые `sub_821B38F8` применяет при сбросе устройства: render state
`0x82CD5E00` (101 запись) и sampler state `0x82CD62C0` (20 записей), по 12 байт
{getter, setter, default}. Сеттер render state `i` ложится в `device+64+4*i`,
сеттер sampler state `j` — в `device+468+4*j`. Inline-`SetRenderState` игры
вызывает `device->RenderStateCall[i](device, value)`.

"writes" — поля тени регистров (см. `D3D_MAP.md`) и копии D3D, куда сеттер
кладёт значение; имена полей из `registers.h` SDK. Сеттеры sampler state пишут
по вычисляемому адресу (`stwx` в fetch-константу слота, `device+1152+24*s`),
поэтому разбор их полей не видит. "no-op" — общий сеттер `82492D68`.

| table | index | setter | default | writes |
|---|---|---|---|---|
| render | 0 | `82492D68` | `00000000` | no-op |
| render | 1 | `82492D68` | `00000003` | no-op |
| render | 2 | `82492D68` | `00000001` | no-op |
| render | 3 | `82492D68` | `00000000` | no-op |
| render | 4 | `82492D68` | `00000000` | no-op |
| render | 5 | `82492D68` | `00000000` | no-op |
| render | 6 | `82492D68` | `3F800000` | no-op |
| render | 7 | `82492D68` | `00000000` | no-op |
| render | 8 | `82492D68` | `00000000` | no-op |
| render | 9 | `82492D68` | `00000000` | no-op |
| render | 10 | `821A2A80` | `00000001` | device+11876 bits 0-31; RB_DEPTHCONTROL.z_enable |
| render | 11 | `821A2AF0` | `00000003` | RB_DEPTHCONTROL.zfunc |
| render | 12 | `821A2AC0` | `00000001` | RB_DEPTHCONTROL.z_write_enable |
| render | 13 | `821A2388` | `00000000` | PA_SU_SC_MODE_CNTL bits 3-10 |
| render | 14 | `821A2358` | `00000006` | PA_SU_SC_MODE_CNTL bits 0-31 |
| render | 15 | `821A23F0` | `00000000` | device+11852 bit 31; RB_BLENDCONTROL2 bit 31; RB_BLENDCONTROL0 bit 31; RB_BLENDCONTROL1 bit 31; RB_BLENDCONTROL3 bit 31 |
| render | 16 | `821A2780` | `00000000` | device+11852 bit 30; RB_BLENDCONTROL0 bit 30; RB_BLENDCONTROL1 bit 30; RB_BLENDCONTROL2 bit 30; RB_BLENDCONTROL3 bit 30 |
| render | 17 | `821A28A8` | `FFFFFFFF` | (no device field) |
| render | 18 | `821A2510` | `00000001` | device+11848 bits 0-4; RB_BLENDCONTROL0; RB_BLENDCONTROL1; RB_BLENDCONTROL2; RB_BLENDCONTROL3 |
| render | 19 | `821A25A0` | `00000000` | device+11848 bits 8-12; RB_BLENDCONTROL0; RB_BLENDCONTROL1; RB_BLENDCONTROL2; RB_BLENDCONTROL3 |
| render | 20 | `821A2480` | `00000000` | device+11848 bits 5-7; RB_BLENDCONTROL0; RB_BLENDCONTROL1; RB_BLENDCONTROL2; RB_BLENDCONTROL3 |
| render | 21 | `821A26A0` | `00000001` | device+11848 bits 16-20; RB_BLENDCONTROL0; RB_BLENDCONTROL1; RB_BLENDCONTROL2; RB_BLENDCONTROL3 |
| render | 22 | `821A2710` | `00000000` | device+11848 bits 24-28; RB_BLENDCONTROL0; RB_BLENDCONTROL1; RB_BLENDCONTROL2; RB_BLENDCONTROL3 |
| render | 23 | `821A2630` | `00000000` | device+11848 bits 21-23; RB_BLENDCONTROL0; RB_BLENDCONTROL1; RB_BLENDCONTROL2; RB_BLENDCONTROL3 |
| render | 24 | `821A23B8` | `00000000` | RB_COLORCONTROL.alpha_test_enable |
| render | 25 | `821A2818` | `00000000` | (no device field) |
| render | 26 | `821A2878` | `00000007` | RB_COLORCONTROL bits 0-31 |
| render | 27 | `821A2B28` | `00000000` | device+11880 bits 0-31; RB_DEPTHCONTROL.stencil_enable |
| render | 28 | `821A2B68` | `00000000` | RB_DEPTHCONTROL.backface_enable |
| render | 29 | `821A2BD0` | `00000000` | RB_DEPTHCONTROL.stencilfail |
| render | 30 | `821A2C08` | `00000000` | RB_DEPTHCONTROL.stencilzfail |
| render | 31 | `821A2C40` | `00000000` | RB_DEPTHCONTROL.stencilzpass |
| render | 32 | `821A2BA0` | `00000007` | RB_DEPTHCONTROL.stencilfunc |
| render | 33 | `821A2D40` | `00000000` | RB_STENCILREFMASK byte 3.stencilref |
| render | 34 | `821A2D60` | `FFFFFFFF` | RB_STENCILREFMASK byte 2.stencilmask |
| render | 35 | `821A2D80` | `FFFFFFFF` | RB_STENCILREFMASK byte 1.stencilwritemask |
| render | 36 | `821A2CA0` | `00000000` | RB_DEPTHCONTROL.stencilfail_bf |
| render | 37 | `821A2CD8` | `00000000` | RB_DEPTHCONTROL.stencilzfail_bf |
| render | 38 | `821A2D10` | `00000000` | RB_DEPTHCONTROL.stencilzpass_bf |
| render | 39 | `821A2C70` | `00000007` | RB_DEPTHCONTROL.stencilfunc_bf |
| render | 40 | `821A2DA0` | `00000000` | RB_STENCILREFMASK_BF byte 3 bits 0-7 |
| render | 41 | `821A2DC0` | `FFFFFFFF` | RB_STENCILREFMASK_BF byte 2 bits 8-15 |
| render | 42 | `821A2DE0` | `FFFFFFFF` | RB_STENCILREFMASK_BF byte 1 bits 16-23 |
| render | 43 | `821A2E00` | `00000000` | PA_CL_CLIP_CNTL; COHER_DEST_BASE_7 |
| render | 44 | `821A3138` | `3F800000` | PA_SU_POINT_SIZE byte 2; PA_SU_POINT_SIZE byte 0 |
| render | 45 | `821A3190` | `3F800000` | PA_SU_POINT_MINMAX byte 2 |
| render | 46 | `821A3128` | `00000000` | device+11884 bits 0-31 |
| render | 47 | `821A31E0` | `42800000` | PA_SU_POINT_MINMAX byte 0 |
| render | 48 | `821A2FD8` | `00000001` | PA_SU_SC_MODE_CNTL.msaa_enable |
| render | 49 | `821A3008` | `FFFFFFFF` | PA_SC_AA_MASK |
| render | 50 | `821A2E50` | `00000000` | device+11856 bits 0-31 |
| render | 51 | `821A2E68` | `00000000` | PA_SU_SC_MODE_CNTL |
| render | 52 | `821A2F30` | `00000000` | PA_SU_SC_MODE_CNTL |
| render | 53 | `821A3028` | `0000000F` | device+11860 bits 0-31; RB_COLOR_MASK bits 0-31 |
| render | 54 | `821A3068` | `0000000F` | device+11864 bits 0-31; RB_COLOR_MASK bits 4-7 |
| render | 55 | `821A30A8` | `0000000F` | device+11868 bits 0-31; RB_COLOR_MASK bits 8-11 |
| render | 56 | `821A30E8` | `0000000F` | device+11872 bits 0-31; RB_COLOR_MASK bits 12-15 |
| render | 57 | `821A38B0` | `00000001` | VGT_HOS_CNTL bits 0-31 |
| render | 58 | `821A3840` | `3F800000` | (no device field) |
| render | 59 | `821A3878` | `3F800000` | (no device field) |
| render | 60 | `821A3230` | `00000000` | SQ_WRAPPING_0 |
| render | 61 | `821A3250` | `00000000` | SQ_WRAPPING_0 |
| render | 62 | `821A3278` | `00000000` | SQ_WRAPPING_0 |
| render | 63 | `821A32A0` | `00000000` | SQ_WRAPPING_0 |
| render | 64 | `821A32C8` | `00000000` | SQ_WRAPPING_0 |
| render | 65 | `821A32F0` | `00000000` | SQ_WRAPPING_0 |
| render | 66 | `821A3318` | `00000000` | SQ_WRAPPING_0 |
| render | 67 | `821A3340` | `00000000` | SQ_WRAPPING_0 bits 28-31 |
| render | 68 | `821A3360` | `00000000` | SQ_WRAPPING_1 |
| render | 69 | `821A3380` | `00000000` | SQ_WRAPPING_1 |
| render | 70 | `821A33A8` | `00000000` | SQ_WRAPPING_1 |
| render | 71 | `821A33D0` | `00000000` | SQ_WRAPPING_1 |
| render | 72 | `821A33F8` | `00000000` | SQ_WRAPPING_1 |
| render | 73 | `821A3420` | `00000000` | SQ_WRAPPING_1 |
| render | 74 | `821A3448` | `00000000` | SQ_WRAPPING_1 |
| render | 75 | `821A3470` | `00000000` | SQ_WRAPPING_1 bits 28-31 |
| render | 76 | `821A3590` | `00000001` | PA_CL_VTE_CNTL; PA_CL_CLIP_CNTL |
| render | 77 | `821A35E0` | `00000000` | device+12028 bits 0-31; RB_COLOR_INFO |
| render | 78 | `821A3678` | `00000000` | device+12032 bits 0-31; RB_COLOR1_INFO |
| render | 79 | `821A3710` | `00000000` | device+12036 bits 0-31; RB_COLOR2_INFO |
| render | 80 | `821A37A8` | `00000000` | device+12040 bits 0-31; RB_COLOR3_INFO |
| render | 81 | `821A38E8` | `00000000` | PA_SU_VTX_CNTL bits 0-31 |
| render | 82 | `821A3920` | `00000000` | PA_SU_SC_MODE_CNTL.multi_prim_ib_ena |
| render | 83 | `821A3950` | `0000FFFF` | VGT_MULTI_PRIM_IB_RESET_INDX bits 0-31 |
| render | 84 | `821A3978` | `00000000` | RB_COLORCONTROL.alpha_to_mask_enable |
| render | 85 | `821A39A8` | `00000087` | RB_COLORCONTROL bits 24-31 |
| render | 86 | `821A39E0` | `40000000` | (no device field) |
| render | 87 | `821A3A10` | `40000000` | (no device field) |
| render | 88 | `821A3A40` | `3F800000` | (no device field) |
| render | 89 | `821A3A68` | `3F800000` | (no device field) |
| render | 90 | `821A3B30` | `00000000` | RB_HIZCONTROL bit 3 |
| render | 91 | `821A3B60` | `00000000` | RB_HIZCONTROL bit 2 |
| render | 92 | `821A3B90` | `00000000` | RB_HIZCONTROL bit 5 |
| render | 93 | `821A3BC0` | `00000000` | RB_HIZCONTROL byte 2 bits 8-15 |
| render | 94 | `821A3BE0` | `00000000` | device+13596 bits 0-31 |
| render | 95 | `821A3BF0` | `00000000` | device+11852 bits 23-29 |
| render | 96 | `821A3A98` | `00000002` | device+11852 bits 20-22 |
| render | 97 | `821A3AC8` | `00000002` | device+11852 bits 17-19 |
| render | 98 | `821A29A8` | `00000001` | PA_SC_LINE_CNTL bit 10 |
| render | 99 | `821A29E0` | `3F800000` | PA_SU_LINE_CNTL byte 2 |
| render | 100 | `821A2A50` | `00000001` | device+23980 |
| sampler | 0 | `821A4388` | `00000000` | (no device field) |
| sampler | 1 | `821A43D8` | `00000000` | (no device field) |
| sampler | 2 | `821A4428` | `00000000` | (no device field) |
| sampler | 3 | `821A4318` | `00000000` | (no device field) |
| sampler | 4 | `821A3DB8` | `00000000` | (no device field) |
| sampler | 5 | `821A3C10` | `00000000` | (no device field) |
| sampler | 6 | `821A3F58` | `00000002` | (no device field) |
| sampler | 7 | `821A4178` | `00000000` | (no device field) |
| sampler | 8 | `821A4218` | `00000000` | (no device field) |
| sampler | 9 | `821A4058` | `00000001` | (no device field) |
| sampler | 10 | `821A3EB8` | `00000000` | (no device field) |
| sampler | 11 | `821A3D10` | `00000000` | (no device field) |
| sampler | 12 | `821A3FB0` | `00000000` | (no device field) |
| sampler | 13 | `821A4298` | `0000000D` | (no device field) |
| sampler | 14 | `821A4478` | `00000000` | (no device field) |
| sampler | 15 | `821A40D0` | `00000000` | (no device field) |
| sampler | 16 | `821A44D0` | `00000000` | (no device field) |
| sampler | 17 | `821A4528` | `00000000` | (no device field) |
| sampler | 18 | `821A4580` | `00000000` | (no device field) |
| sampler | 19 | `821A45D8` | `00000001` | (no device field) |
