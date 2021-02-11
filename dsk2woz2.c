//
//  dsk2woz2
//
//  By Ben Zotto. Copyright (c) 2021.
//  Portions of this program are derived from dsk2woz, copyright (c) 2018 Thomas Harte.
//
//  This utility will read a standard Apple II 5.25" DSK disk image file (16 sector, 35 track)
//  in DOS 3.3 (or ProDOS) sector format, and emit a writeable WOZ2 image file.
//
//  The WOZ images are compatible with the Applesauce FDC, and readable by various emulators.
//  Specification is here: https://applesaucefdc.com/woz/reference2/
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//
// Helpful constants and types
//

#define CREATOR_NAME        "dsk2woz2"

#define TRACKS_PER_DISK     35
#define SECTORS_PER_TRACK   16
#define BYTES_PER_SECTOR    256
#define BYTES_PER_TRACK     (SECTORS_PER_TRACK * BYTES_PER_SECTOR)
#define DSK_IMAGE_SIZE      (TRACKS_PER_DISK * BYTES_PER_TRACK)

#define BITS_BLOCKS_PER_TRACK       13
#define BITS_BLOCK_SIZE             512
#define BITS_TRACK_SIZE             (BITS_BLOCKS_PER_TRACK * BITS_BLOCK_SIZE)
#define BITS_SECTOR_CONTENTS_SIZE   343
#define WOZ_HEADER_SIZE             12

#define DOS_VOLUME_NUMBER           254
#define TRACK_LEADER_SYNC_COUNT     64

typedef enum _dsk_sector_format {
    dsk_sector_format_dos_3_3 = 0,
    dsk_sector_format_prodos = 1
} dsk_sector_format;

typedef struct _woz_chunk {
    uint32_t name;
    size_t data_length;
    uint8_t data[0];
} woz_chunk;

//
// Forward declarations for utility routines
//

static woz_chunk * create_info_chunk(void);
static woz_chunk * create_tmap_chunk(void);
static woz_chunk * create_trks_chunk(uint8_t * track_data, uint32_t valid_bits_per_track);
static woz_chunk * create_writ_chunk(uint8_t * track_data, uint32_t valid_bits_per_track);
static size_t total_chunk_size(woz_chunk * chunk);
static size_t write_chunk(uint8_t * dest, woz_chunk * chunk);
static void free_chunk(woz_chunk * chunk);

static void write_uint8(uint8_t * dest, uint8_t value);
static void write_uint16(uint8_t * dest, uint16_t value);
static void write_uint32(uint8_t * dest, uint32_t value);
static void write_uint32_be(uint8_t * dest, uint32_t value);
static void write_utf8(uint8_t * dest, const char * utf8string, int n);

static size_t encode_bits_for_track(uint8_t * dest, uint8_t * src, int track_number, dsk_sector_format sector_format);

static uint32_t crc32(uint32_t crc, const void * buf, size_t size);

//
// Utility entry point
//

int main(int argc, const char * argv[])
{
    if (argc != 3) {
        printf("USAGE: dsk2woz2 input.dsk output.woz\n");
        return -1;
    }

    // Read the input DSK file.
    FILE * const dsk_file = fopen(argv[1], "rb");
    if (!dsk_file) {
        printf("ERROR: could not open %s for reading\n", argv[1]);
        return -2;
    }
    
    uint8_t dsk[DSK_IMAGE_SIZE];
    const size_t bytes_read = fread(dsk, 1, DSK_IMAGE_SIZE, dsk_file);
    fclose(dsk_file);
    
    if (bytes_read != DSK_IMAGE_SIZE) {
        printf("ERROR: file %s does not appear to be a 16-sector 5.25\" disk image", argv[1]);
        return -2;
    }
    
    // Assume the standard DOS 3.3 sector format unless the input filename ends in
    // .po, which indicates ProDOS sectoring. (The sector format of the image is not
    // necessarily the same as the formatting of the disk.)
    dsk_sector_format sector_format = dsk_sector_format_dos_3_3;
    if (strlen(argv[1]) > 3 &&
        strncmp(&(argv[1][strlen(argv[1])-3]), ".po", 3) == 0) {
        sector_format = dsk_sector_format_prodos;
    }
    
    // Build the encoded track data. We do this up front because we'll need to access it within
    // both the TRKS and the WRIT chunk creation.
    uint8_t track_data[TRACKS_PER_DISK * BITS_TRACK_SIZE];
    size_t valid_bits_per_track = 0;  // Re-set each loop, we just need to know the fixed value.
    for (int t = 0; t < TRACKS_PER_DISK; t++) {
        valid_bits_per_track = encode_bits_for_track(&track_data[t * BITS_TRACK_SIZE],
                                                     &dsk[t * BYTES_PER_TRACK],
                                                     t, sector_format);
    }
    
    // Build the chunks.
    woz_chunk * info_chunk = create_info_chunk();
    woz_chunk * tmap_chunk = create_tmap_chunk();
    woz_chunk * trks_chunk = create_trks_chunk(track_data, (uint32_t)valid_bits_per_track);
    woz_chunk * writ_chunk = create_writ_chunk(track_data, (uint32_t)valid_bits_per_track);

    if (!info_chunk || !tmap_chunk || !trks_chunk || !writ_chunk) {
        printf("ERROR: memory allocation failed");
        return -2;
    }
    
    // Create the final output buffer.
    size_t woz_image_size = WOZ_HEADER_SIZE +
                            total_chunk_size(info_chunk) +
                            total_chunk_size(tmap_chunk) +
                            total_chunk_size(trks_chunk) +
                            total_chunk_size(writ_chunk);
    uint8_t * woz = malloc(woz_image_size);
    if (!woz) {
        printf("ERROR: memory allocation failed");
        return -2;
    }
        
    // Emit the header. Leave the CRC slot empty; will write that last.
    write_uint32_be(woz, 'WOZ2');                  // 'WOZ2' magic number
    woz[4] = 0xFF;                                 // Marker to ensure high bits present
    woz[5] = '\n'; woz[6] = '\r'; woz[7] = '\n';   // LF CR LF to ensure no text transforms
    
    // Copy the chunk data in order.
    size_t output_index = WOZ_HEADER_SIZE;
    output_index += write_chunk(&woz[output_index], info_chunk);
    output_index += write_chunk(&woz[output_index], tmap_chunk);
    output_index += write_chunk(&woz[output_index], trks_chunk);
    output_index += write_chunk(&woz[output_index], writ_chunk);

    // Unnecessary boy scoutery...
    free_chunk(info_chunk);
    free_chunk(tmap_chunk);
    free_chunk(trks_chunk);
    free_chunk(writ_chunk);

    // Compute the overall CRC of everthing after the header, and write it in.
    uint32_t crc = crc32(0, &woz[WOZ_HEADER_SIZE], woz_image_size - WOZ_HEADER_SIZE);
    write_uint32(&woz[8], crc);
    
    // Write the output file
    FILE * const woz_file = fopen(argv[2], "wb");
    if (!woz_file) {
        printf("ERROR: Could not open %s for writing\n", argv[2]);
        return -5;
    }

    size_t bytes_written = fwrite(woz, 1, woz_image_size, woz_file);
    fclose(woz_file);
    free(woz);
    
    if(bytes_written != woz_image_size) {
        printf("ERROR: Could not write full WOZ image\n");
        return -6;
    }

    return 0;
}

//
// Chunk creation and writing utility routines
//

woz_chunk * create_chunk(uint32_t name, size_t data_length)
{
    woz_chunk * chunk = calloc(sizeof(woz_chunk) + data_length, 1);
    if (chunk) {
        chunk->name = name;
        chunk->data_length = data_length;
    }
    return chunk;
}

static
woz_chunk * create_info_chunk()
{
    woz_chunk * chunk = create_chunk('INFO', 60);
    if (!chunk) { return NULL; }
    write_uint8(&chunk->data[0], 2); // INFO version 2
    write_uint8(&chunk->data[1], 1); // Disk Type (1 = 5.25)
    write_uint8(&chunk->data[2], 0); // Write Protected
    write_uint8(&chunk->data[3], 0); // Synchronized
    write_uint8(&chunk->data[4], 1); // Cleaned
    write_utf8(&chunk->data[5], CREATOR_NAME, 32);  // Creator
    write_uint8(&chunk->data[37], 1); // Disk sides (1 for 5.25")
    write_uint8(&chunk->data[38], 1); // Boot sector format (1 = 16-sector)
    write_uint8(&chunk->data[39], 32); // Optimal bit timing (32 = 4 uS standard)
    write_uint16(&chunk->data[40], 0); // Compatibile hardware (0 = unknown)
    write_uint16(&chunk->data[42], 0); // Required RAM (0 = unknown)
    write_uint16(&chunk->data[44], BITS_BLOCKS_PER_TRACK); // largest track in blocks
    return chunk;
}

static
woz_chunk * create_tmap_chunk()
{
    woz_chunk * chunk = create_chunk('TMAP', 160);
    if (!chunk) { return NULL; }
    size_t byte_index = 0;
    // We will write all bytes of this chunk; unused entries get 0xFF (not zero).
    for (int t = 0; t < 160; t++) {
        // Each entry is a quarter-track, 0.00, 0.25, 0.50, 0.75, 1.00 and so on.
        // Standard disks have "visible" data in the dead-center track (x.00) but
        // also a quarter-track to either side. So write the nominal track at the .00,
        // as well as the +0.25 and the -0.25 position relative to it. We only
        // do this for the tracks we care about, and cut it off one quarter track early
        // so we don't emit an erroneous "track 35" for the 34.75 position.
        if (t < (TRACKS_PER_DISK * 4) - 1) {
            int nominal_track = t / 4;
            switch (t % 4) {
                case 0:
                case 1:
                    write_uint8(&chunk->data[byte_index++], nominal_track);
                    break;
                case 2:
                    write_uint8(&chunk->data[byte_index++], 0xFF);
                    break;
                case 3:
                    write_uint8(&chunk->data[byte_index++], nominal_track + 1);
                    break;
                default:
                    break;
            }
        } else {
            write_uint8(&chunk->data[byte_index++], 0xFF);
        }
    }
    return chunk;
}

static
woz_chunk * create_trks_chunk(uint8_t * track_data, uint32_t valid_bits_per_track)
{
    woz_chunk * chunk = create_chunk('TRKS', (160 * 8) + (TRACKS_PER_DISK * BITS_TRACK_SIZE));
    if (!chunk) { return NULL; }

    // Write each mandatory TRK structure (8 bytes each)
    size_t byte_index = 0;

    // !!! starting_block is relative to the start of the file !!! This means we depend on
    // writing the chunks in a fixed order up to this point (INFO, TMAP, TRKS, ...).
    uint16_t starting_block = 3;
    for (int i = 0 ; i < TRACKS_PER_DISK; i++) {
        write_uint16(&chunk->data[byte_index], starting_block);
        byte_index += 2;
        write_uint16(&chunk->data[byte_index], BITS_BLOCKS_PER_TRACK);
        byte_index += 2;
        write_uint32(&chunk->data[byte_index], valid_bits_per_track);
        byte_index += 4;
        starting_block += BITS_BLOCKS_PER_TRACK;
    }
    // Copy the track bits themselves. This should already be aligned and complete so
    // just do a blind copy. There are always 160 tracks' worth of TRK entries, even though
    // the vast majority are all zeroes, and the BITS always starts at offset 1280, following
    // the TRK table.
    memcpy(&chunk->data[1280], track_data, (TRACKS_PER_DISK * BITS_TRACK_SIZE));
    return chunk;
}

static
woz_chunk * create_writ_chunk(uint8_t * track_data, uint32_t valid_bits_per_track)
{
    woz_chunk * chunk = create_chunk('WRIT', TRACKS_PER_DISK * 20);
    if (!chunk) { return NULL; }
    size_t byte_index = 0;
    for (int t = 0; t < TRACKS_PER_DISK; t++) {
        write_uint8(&chunk->data[byte_index++], t * 4); // track to write (always the x.00)
        write_uint8(&chunk->data[byte_index++], 1);     // 1 command in the write array
        write_uint8(&chunk->data[byte_index++], 0x00);  // no additional flags
        byte_index++;                                   // reserved (0)
        
        uint8_t * track_bits = &track_data[t * BITS_TRACK_SIZE];
        size_t length_for_crc = (valid_bits_per_track + 7) / 8;
        uint32_t crc = crc32(0, track_bits, length_for_crc);
        write_uint32(&chunk->data[byte_index], crc);    // BITS checksum
        byte_index += 4;
        uint32_t track_leader_sync_bits = TRACK_LEADER_SYNC_COUNT * 10;
        write_uint32(&chunk->data[byte_index], track_leader_sync_bits); // Don't rewrite the track leader
        byte_index += 4;
        write_uint32(&chunk->data[byte_index], valid_bits_per_track - track_leader_sync_bits);   // Bit count
        byte_index += 4;
        write_uint8(&chunk->data[byte_index++], 0xFF);  // Leader nibble
        write_uint8(&chunk->data[byte_index++], 10);    // Leader nibble bit count
        // Leader count. I'm not sure why this is 0, but mimics Applesauce save-as-WOZ output:
        write_uint8(&chunk->data[byte_index++], 0);
        byte_index++;                                   // padding (0)
    }
    return chunk;
}

static
size_t total_chunk_size(woz_chunk * chunk)
{
    return 4 + 4 + chunk->data_length;
}

// !! DO NOT mess up knowing how much runway remains in dest.
static
size_t write_chunk(uint8_t * dest, woz_chunk * chunk)
{
    write_uint32_be(dest, chunk->name);
    write_uint32(&dest[4], (uint32_t)chunk->data_length);
    memcpy(&dest[8], chunk->data, chunk->data_length);
    return 8 + chunk->data_length;
}

static
void free_chunk(woz_chunk * chunk)
{
    free(chunk);
}

static
void write_uint8(uint8_t * dest, uint8_t value)
{
    *dest = value;
}

static
void write_uint16(uint8_t * dest, uint16_t value)
{
    dest[0] = value & 0xFF;
    dest[1] = (value >> 8) & 0xFF;
}

static
void write_uint32(uint8_t * dest, uint32_t value)
{
    dest[0] = value & 0xFF;
    dest[1] = (value >> 8) & 0xFF;
    dest[2] = (value >> 16) & 0xFF;
    dest[3] = (value >> 24) & 0xFF;
}

static
void write_uint32_be(uint8_t * dest, uint32_t value)
{
    dest[0] = (value >> 24) & 0xFF;
    dest[1] = (value >> 16) & 0xFF;
    dest[2] = (value >> 8) & 0xFF;
    dest[3] = value & 0xFF;
}

// This routine expects utf8string to be both a valid UTF string and
// NUL terminated. If the string is longer than n character, it will be
// truncated. The resulting string will not be NUL terminated but will be
// padded with space characters.
static
void write_utf8(uint8_t * dest, const char * utf8string, int n)
{
    int l = (int)strlen(utf8string);
    for (int i = 0; i < n; i++) {
        dest[i] = (i < l) ? utf8string[i] : 0x20;
    }
}

//
// Track encoding and writing routines
//

static
size_t bits_write_byte(uint8_t * buffer, size_t index, int value)
{
    size_t shift = index & 7;
    size_t byte_position = index >> 3;

    buffer[byte_position] |= value >> shift;
    if (shift) {
        buffer[byte_position + 1] |= value << (8 - shift);
    }
    
    return index + 8;
}

// Writes a byte in 4-and-4
static
size_t bits_write_4_and_4(uint8_t * buffer, size_t index, int value)
{
    index = bits_write_byte(buffer, index, (value >> 1) | 0xAA);
    index = bits_write_byte(buffer, index, value | 0xAA);
    return index;
}

// Writes a 6-and-2 sync word
static
size_t bits_write_sync(uint8_t * buffer, size_t index)
{
    index = bits_write_byte(buffer, index, 0xFF);
    return index + 2; // Skip two bits, i.e. leave them as 0s.
}

// Encodes a 256-byte sector buffer into a 343 byte 6-and-2 encoding of same
static
void encode_6_and_2(uint8_t * dest, const uint8_t * src)
{
    const uint8_t six_and_two_mapping[] = {
        0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6,
        0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
        0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc,
        0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
        0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
        0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
        0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6,
        0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
    };

    // Fill in byte values: the first 86 bytes contain shuffled
    // and combined copies of the bottom two bits of the sector
    // contents; the 256 bytes afterwards are the remaining
    // six bits.
    const uint8_t bit_reverse[] = {0, 2, 1, 3};
    for (int c = 0; c < 84; c++) {
        dest[c] =
            bit_reverse[src[c] & 3] |
            (bit_reverse[src[c+86] & 3] << 2) |
            (bit_reverse[src[c+172] & 3] << 4);
    }
    dest[84] =
        (bit_reverse[src[84] & 3] << 0) |
        (bit_reverse[src[170] & 3] << 2);
    dest[85] =
        (bit_reverse[src[85] & 3] << 0) |
        (bit_reverse[src[171] & 3] << 2);

    for (int c = 0; c < 256; c++) {
        dest[86+c] = src[c] >> 2;
    }

    // Exclusive OR each byte with the one before it.
    dest[342] = dest[341];
    int location = 342;
    while (location > 1) {
        location--;
        dest[location] ^= dest[location-1];
    }

    // Map six-bit values up to full bytes.
    for (int c = 0; c < 343; c++) {
        dest[c] = six_and_two_mapping[dest[c]];
    }
}

static
size_t encode_bits_for_track(uint8_t * dest, uint8_t * src, int track_number, dsk_sector_format sector_format)
{
    size_t bit_index = 0;
    memset(dest, 0, BITS_TRACK_SIZE);
    
    // Write 64 sync words
    for (int i = 0; i < TRACK_LEADER_SYNC_COUNT; i++) {
        bit_index = bits_write_sync(dest, bit_index);
    }

    // Write out the sectors in physical order. We will select the appopriate logical
    // input data for each physical output sector.
    for (int s = 0; s < SECTORS_PER_TRACK; s++) {
        
        //
        // Sector header
        //
        
        // Prologue
        bit_index = bits_write_byte(dest, bit_index, 0xD5);
        bit_index = bits_write_byte(dest, bit_index, 0xAA);
        bit_index = bits_write_byte(dest, bit_index, 0x96);
        
        // Volume, track, sector and checksum, all in 4-and-4 format
        bit_index = bits_write_4_and_4(dest, bit_index, DOS_VOLUME_NUMBER);
        bit_index = bits_write_4_and_4(dest, bit_index, track_number);
        bit_index = bits_write_4_and_4(dest, bit_index, s);
        bit_index = bits_write_4_and_4(dest, bit_index, DOS_VOLUME_NUMBER ^ track_number ^ s);
        
        // Epilogue
        bit_index = bits_write_byte(dest, bit_index, 0xDE);
        bit_index = bits_write_byte(dest, bit_index, 0xAA);
        bit_index = bits_write_byte(dest, bit_index, 0xEB);

        // Write 7 sync words.
        for (int i = 0; i < 7; i++) {
            bit_index = bits_write_sync(dest, bit_index);
        }

        //
        // Sector body
        //
        
        // Prologue
        bit_index = bits_write_byte(dest, bit_index, 0xD5);
        bit_index = bits_write_byte(dest, bit_index, 0xAA);
        bit_index = bits_write_byte(dest, bit_index, 0xAD);

        // Figure out which logical sector goes into this physical sector.
        int logical_sector;
        if (s == 0x0F) {
            logical_sector = 0x0F;
        } else {
            int multiplier = (sector_format == dsk_sector_format_prodos) ? 8 : 7;
            logical_sector = (s * multiplier) % 15;
        }

        // Finally, the actual contents! Encode the buffer, then write them.
        uint8_t encoded_contents[BITS_SECTOR_CONTENTS_SIZE];
        encode_6_and_2(encoded_contents, &src[logical_sector * BYTES_PER_SECTOR]);
        for (int i = 0; i < BITS_SECTOR_CONTENTS_SIZE; i++) {
            bit_index = bits_write_byte(dest, bit_index, encoded_contents[i]);
        }
        
        // Epilogue
        bit_index = bits_write_byte(dest, bit_index, 0xDE);
        bit_index = bits_write_byte(dest, bit_index, 0xAA);
        bit_index = bits_write_byte(dest, bit_index, 0xEB);

        // Conclude the track
        if (s < (SECTORS_PER_TRACK - 1)) {
            // Write 16 sync words
            for (int i = 0; i < 16; i++) {
                bit_index = bits_write_sync(dest, bit_index);
            }
        } else {
            bit_index = bits_write_byte(dest, bit_index, 0xFF);
        }
    }
    
    // Any remaining bytes in the destination buffer will remain cleared to zero and
    // function as padding to the nearest 512-byte block.
    
    // Return the current bit index, which is equal to the number of valid written bits
    return bit_index;
}

//
// CRC routine and table.
// Gary S. Brown, 1986.
// Copied from https://applesaucefdc.com/woz/reference2/
//

static uint32_t crc32_tab[] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

static
uint32_t crc32(uint32_t crc, const void *buf, size_t size)
{
    const uint8_t *p;
    p = buf;
    crc = crc ^ ~0U;
    while (size--)
    crc = crc32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ ~0U;
}
