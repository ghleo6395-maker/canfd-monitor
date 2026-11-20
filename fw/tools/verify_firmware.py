#!/usr/bin/env python3
"""Validate flash load image, vectors, ELF ABI and Intel HEX consistency."""
import argparse, pathlib, struct, subprocess, re
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('base', nargs='?', default='build/revd/canfd-revd', type=pathlib.Path)
parser.add_argument('--board', choices=('revd', 'reve'), help='Require the linked board implementation')
args = parser.parse_args()
base = args.base
bin_data=base.with_suffix('.bin').read_bytes()
assert 1024<len(bin_data)<=2*1024*1024
sp,reset=struct.unpack_from('<II',bin_data)
assert sp==0x20020000,hex(sp)
assert reset&1 and 0x08000000<reset<0x08000000+len(bin_data),hex(reset)
flash={};upper=0;eof=False
for line in base.with_suffix('.hex').read_text().splitlines():
 assert line.startswith(':')
 row=bytes.fromhex(line[1:]);assert sum(row)&255==0
 n,addr,kind=row[0],int.from_bytes(row[1:3],'big'),row[3]
 payload=row[4:4+n];assert len(row)==n+5
 if kind==0:
  for i,b in enumerate(payload):
   a=upper+addr+i;assert a not in flash;flash[a]=b
 elif kind==4:upper=int.from_bytes(payload,'big')<<16
 elif kind==1:eof=True
 elif kind==5:assert int.from_bytes(payload,'big')==reset
 else:raise AssertionError(f'Unsupported record {kind}')
assert eof and min(flash)==0x08000000
assert max(flash)<0x08200000
for a,b in flash.items():assert bin_data[a-0x08000000]==b
hdr=subprocess.check_output(['arm-none-eabi-readelf','-h','-A',str(base.with_suffix('.elf'))],text=True)
assert 'Machine:                           ARM' in hdr
assert 'hard-float ABI' in hdr and 'Tag_CPU_arch: v7E-M' in hdr
syms=subprocess.check_output(['arm-none-eabi-nm','-n',str(base.with_suffix('.elf'))],text=True)
symbols = {line.split()[-1]: int(line.split()[0],16)
           for line in syms.splitlines() if len(line.split())==3}
symbol_types = {line.split()[-1]: line.split()[1]
                for line in syms.splitlines() if len(line.split())==3}
rtos = 'revd_rtos_start' in symbols
board = 'reve' if 'reve_power_init' in symbols else 'revd'
if args.board:
    assert board == args.board, f'Board mismatch: requested {args.board}, linked image is {board}'
if board == 'reve':
    assert not rtos, 'Rev E must be bare metal'
    for name in ['reve_power_task', 'reve_power_tick', 'reve_power_usb_reset', 'input_policy_step']:
        assert name in symbols, f'Rev E image missing {name}'
required = ['Reset_Handler','main','OTG_FS_IRQHandler','storage_sd_task',
            'lv_st7796_create','tud_msc_read10_cb','board_can_task',
            'SDMMC1_IRQHandler','DMA1_Stream0_IRQHandler','SPI4_IRQHandler',
            'FDCAN1_IT0_IRQHandler','FDCAN2_IT0_IRQHandler',
            'HAL_SD_ReadBlocks_DMA','HAL_SD_WriteBlocks_DMA',
            'HAL_SPI_Transmit_DMA','revd_io_wait_poll','revd_sdmmc_cmd_wait_poll',
            'bench_tick','bench_execute','port_screen_redraw','board_boot_id',
            'HAL_RNG_GenerateRandomNumber','logfmt_line_size']
for name in required: assert name in symbols, name
if rtos:
    for name in ['SVC_Handler','PendSV_Handler','SysTick_Handler','xPortSysTickHandler',
                 'TIM7_IRQHandler','vTaskStartScheduler','xTaskCreateStatic',
                 'revd_rtos_start','revd_rtos_bridges_init','revd_core_boot',
                 'revd_core_step','revd_ui_step','revd_usb_transport_step',
                 'revd_storage_owner_init','revd_storage_owner_step',
                 'revd_rtos_notify_core_from_isr_deferred',
                 'revd_rtos_notify_storage_from_isr','revd_rtos_notify_ui_from_isr']:
        assert name in symbols, f'RTOS image missing {name}'
        assert symbol_types[name] not in ['w','W'], f'Weak RTOS stub is not a completed bridge: {name}'
    for offset,name in [(11,'SVC_Handler'),(14,'PendSV_Handler'),(15,'SysTick_Handler')]:
        addr=struct.unpack_from('<I',bin_data,offset*4)[0]
        assert addr == symbols[name]|1,(name,hex(addr))
    for name in ['pvPortMalloc','vPortFree','xTaskCreate','xQueueGenericCreate']:
        assert name not in symbols, f'Unexpected dynamic kernel object allocation: {name}'
for name in ['HAL_SD_ReadBlocks','HAL_SD_WriteBlocks']:
    assert name not in symbols, f'Unexpected polling SD transfer: {name}'
fw = pathlib.Path(__file__).resolve().parents[1]
header=(fw/'vendor/cmsis_device/Include/stm32h743xx.h').read_text()
for name in ['SDMMC1','DMA1_Stream0','SPI4','FDCAN1_IT0','FDCAN2_IT0','OTG_FS']+(['TIM7'] if rtos else []):
    irq=int(re.search(r'\b'+name+r'_IRQn\s*=\s*(\d+)',header)[1])
    addr=struct.unpack_from('<I',bin_data,(16+irq)*4)[0]
    assert addr == symbols[name+'_IRQHandler']|1,(name,hex(addr))
assert 0x30000000<=symbols['_sd2bss']<=symbols['_ed2bss']<=0x30048000
assert 0x38000000<=symbols['_sd3bss']<=symbols['_ed3bss']<=0x38010000
assert 0x20000000<=symbols['_sdtcmbss']<=symbols['_edtcmbss']<=0x2001C000
assert 0x24000000<=symbols['_sbss']<=symbols['_ebss']<=0x24080000
assert symbols['_stack_limit']==0x2001C000
if not rtos:
    # A successful link must include the shipped device features, not the old
    # target placeholders or a simulated SD/backend accidentally linked in.
    for name in ['ui_tx_channel_send','local_input_open','local_record_save_open',
                 'file_port_rename','sp_json_decode','sp_json_encode','sp_load',
                 'txseq_realtime_tick','app_realtime_poll','app_tx_complete']:
        assert name in symbols, f'Missing production device feature: {name}'
    for name in ['env_reset','env_log_rename','sim_reset','sim_tick']:
        assert name not in symbols, f'Unexpected simulator backend: {name}'
# Real DMA payload staging buffers must never land in inaccessible DTCM.
for line in syms.splitlines():
    fields=line.split()
    if len(fields)!=3: continue
    addr=int(fields[0],16); name=fields[-1]
    if name in ['dma_buffer','dma_command','draw_buffer']:
        assert (0x24000000<=addr<0x24080000) or (0x30000000<=addr<0x30048000),(name,hex(addr))
    if name=='frame_queue': assert 0x20000000<=addr<0x2001C000
if rtos:
    sized=subprocess.check_output(['arm-none-eabi-nm','-S',str(base.with_suffix('.elf'))],text=True)
    by_size={f[-1]:(int(f[0],16),int(f[1],16)) for line in sized.splitlines()
             if len(f:=line.split())==4}
    for name,size in [('core_stack',8192),('storage_stack',12288),('usb_stack',6144)]:
        addr,n=by_size[name]
        assert n==size and 0x20000000<=addr and addr+n<=0x2001C000,(name,hex(addr),n)
    for name,size in [('ui_stack',16384),('idle_stack',1024)]:
        addr,n=by_size[name]
        assert n==size and 0x24000000<=addr and addr+n<=0x24080000,(name,hex(addr),n)
asm=subprocess.check_output(['arm-none-eabi-objdump','-d','--disassemble=Reset_Handler',str(base.with_suffix('.elf'))],text=True)
assert asm.index('dsb')<asm.index('<SystemInit>'), 'Power hold must precede C init'
kind='FreeRTOS static 4-task B' if rtos else 'bare-metal A'
print(f'PASS: {board}; {kind}; {len(bin_data)} byte image; initial SP={sp:#x}, Reset={reset:#x}; ARM Cortex-M7 hard-float; BIN/HEX agree; DMA SD/SPI and CAN IRQ vectors linked; D2/D3/DTCM bounds valid; early PE2 hold precedes SystemInit.')
