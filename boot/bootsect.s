;   bootsect.s		(C) 1991 Linus Torvalds
; bootsect 由 bios 加载到 0x7c00, bootsect 将自己移动到 0x90000, 并且跳转到那里。
; 然后它利用 bios 的中断将 setup 加载到 0x90200，将系统加载到 0x10000。

; SYS_SIZE 是要加载的系统的大小，大小要 x16。
; 0x3000 is 0x30000 bytes = 196kB, 足够存储当前的版本的 linux 了。
SYSSIZE = 0x3000

.globl begtext, begdata, begbss, endtext, enddata, endbss
.text
begtext:
.data
begdata:
.bss
begbss:
.text

SETUPLEN = 4				; nr of setup-sectors
BOOTSEG  = 0x07c0			; original address of boot-sector
INITSEG  = 0x9000			; we move boot here - out of the way
SETUPSEG = 0x9020			; setup starts here
SYSSEG   = 0x1000			; system loaded at 0x10000 (65536).
ENDSEG   = SYSSEG + SYSSIZE		; where to stop loading

; ROOT_DEV:	0x200 - floppy 作为 boot.
;		0x301 - 第一个硬盘的第一个分区
ROOT_DEV = 0x306	; 硬盘根设备号。0x306 其中0x3是主设备号，表示HD，6 是第 6 个分区，
	; 0-4 是第一个硬盘的 5 个分区；5-9 为第二个硬盘的 5 个分区，6 则表示第二个硬盘的第 1 个可用分区
	; 一般一个硬盘的第 1 个分区为硬盘标识，其余 4 个为可用分区。

entry start
; 将自身由 0x7c00 移动到 0x90000 处。
start:
	mov	ax,#BOOTSEG
	mov	ds,ax
	mov	ax,#INITSEG
	mov	es,ax
	mov	cx,#256		; 拷贝 256 次
	sub	si,si
	sub	di,di
	rep
	movw
	jmpi	go,INITSEG

go:	mov	ax,cs
	mov	ds,ax
	mov	es,ax
; put stack at 0x9ff00.
	mov	ss,ax
	mov	sp,#0xFF00		; arbitrary value >>512

; load the setup-sectors directly after the bootblock.
; Note that 'es' is already set up.

load_setup:
	mov	dx,#0x0000		; drive 0, head 0
	mov	cx,#0x0002		; sector 2, track 0
	mov	bx,#0x0200		; address = 512, in INITSEG
	mov	ax,#0x0200+SETUPLEN	; service 2, nr of sectors
	int	0x13			; read it
	jnc	ok_load_setup		; ok - continue
	mov	dx,#0x0000
	mov	ax,#0x0000		; reset the diskette
	int	0x13
	j	load_setup

ok_load_setup:

; Get disk drive parameters, specifically nr of sectors/track

	mov	dl,#0x00
	mov	ax,#0x0800		; AH=8 is get drive parameters
	int	0x13
	mov	ch,#0x00
	seg cs
	mov	sectors,cx
	mov	ax,#INITSEG
	mov	es,ax

; Print some inane message

	mov	ah,#0x03		; read cursor pos
	xor	bh,bh
	int	0x10
	
	mov	cx,#24
	mov	bx,#0x0007		; page 0, attribute 7 (normal)
	mov	bp,#msg1
	mov	ax,#0x1301		; write string, move cursor
	int	0x10

; ok, we've written the message, now
; we want to load the system (at 0x10000)

	mov	ax,#SYSSEG
	mov	es,ax		; segment of 0x010000
	call	read_it
	call	kill_motor

; After that we check which root-device to use. If the device is
; defined (!= 0), nothing is done and the given device is used.
; Otherwise, either /dev/PS0 (2,28) or /dev/at0 (2,8), depending
; on the number of sectors that the BIOS reports currently.

	seg cs
	mov	ax,root_dev
	cmp	ax,#0
	jne	root_defined
	seg cs
	mov	bx,sectors
	mov	ax,#0x0208		; /dev/ps0 - 1.2Mb
	cmp	bx,#15
	je	root_defined
	mov	ax,#0x021c		; /dev/PS0 - 1.44Mb
	cmp	bx,#18
	je	root_defined
undef_root:
	jmp undef_root
root_defined:
	seg cs
	mov	root_dev,ax

; after that (everyting loaded), we jump to
; the setup-routine loaded directly after
; the bootblock:

	jmpi	0,SETUPSEG

; This routine loads the system at address 0x10000, making sure
; no 64kB boundaries are crossed. We try to load it as fast as
; possible, loading whole tracks whenever we can.
;
; in:	es - starting address segment (normally 0x1000)
;
sread:	.word 1+SETUPLEN	; sectors read of current track
head:	.word 0			; current head
track:	.word 0			; current track

read_it:
	mov ax,es
	test ax,#0x0fff
die:	jne die			; es must be at 64kB boundary
	xor bx,bx		; bx is starting address within segment
rp_read:
	mov ax,es
	cmp ax,#ENDSEG		; have we loaded all yet?
	jb ok1_read
	ret
ok1_read:
	seg cs
	mov ax,sectors
	sub ax,sread
	mov cx,ax
	shl cx,#9
	add cx,bx
	jnc ok2_read
	je ok2_read
	xor ax,ax
	sub ax,bx
	shr ax,#9
ok2_read:
	call read_track
	mov cx,ax
	add ax,sread
	seg cs
	cmp ax,sectors
	jne ok3_read
	mov ax,#1
	sub ax,head
	jne ok4_read
	inc track
ok4_read:
	mov head,ax
	xor ax,ax
ok3_read:
	mov sread,ax
	shl cx,#9
	add bx,cx
	jnc rp_read
	mov ax,es
	add ax,#0x1000
	mov es,ax
	xor bx,bx
	jmp rp_read

read_track:
	push ax
	push bx
	push cx
	push dx
	mov dx,track
	mov cx,sread
	inc cx
	mov ch,dl
	mov dx,head
	mov dh,dl
	mov dl,#0
	and dx,#0x0100
	mov ah,#2
	int 0x13
	jc bad_rt
	pop dx
	pop cx
	pop bx
	pop ax
	ret
bad_rt:	mov ax,#0
	mov dx,#0
	int 0x13
	pop dx
	pop cx
	pop bx
	pop ax
	jmp read_track

/*
 * This procedure turns off the floppy drive motor, so
 * that we enter the kernel in a known state, and
 * don't have to worry about it later.
 */
kill_motor:
	push dx
	mov dx,#0x3f2
	mov al,#0
	outb
	pop dx
	ret

sectors:
	.word 0

msg1:
	.byte 13,10
	.ascii "Loading system ..."
	.byte 13,10,13,10

; 由于 bootsect 上来第一件事就是将自己移动到 0x90000 处，故当前位置变为 0x90000+508=0x901FC
.org 508
root_dev:
	.word ROOT_DEV		; 0x0306，根设备号，第二个磁盘第一个扇区
boot_flag:
	.word 0xAA55		; 结尾标志

.text
endtext:
.data
enddata:
.bss
endbss:
