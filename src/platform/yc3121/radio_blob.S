/* The Bluetooth core's program, cut from the stock app by make
 * (make/8bitdo-ultimate2c-bluetooth.mk): RADIO_BLOB is its path. */
	.section .rodata.radio_blob, "a"
	.balign 4
	.global radio_blob, radio_blob_end
radio_blob:
	.incbin RADIO_BLOB
radio_blob_end:
