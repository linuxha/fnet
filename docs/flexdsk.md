# Flex Disk Image

Sample defines and structs for the Flex Disk Image

```C
#define SECTOR_SIZE         256
#define SIR_SIZE            24
#define SIR_OFFSET          16
#define MAX_VOL_NAME_LEN    11
#define DEFAULT_VOL_NUMBER  1
#define MAX_TRACKS          256     // Maximum tracks allowed (0-255)
#define MAX_SECTORS         255     // Maximum sectors allowed (1-255)
#define MIN_SECTORS         5       // Minimum sectors required (1-4 special, 5+ directory)
#define DIR_ENTRY_SIZE      24

// Sector structure for data interpretation
typedef struct{
    uint8_t  next_track;        // Points to next track in the link (Offset +0)
    uint8_t  next_sector;       // Points to next sector in the link (Offset +1)
    uint8_t  File_logicalHi;    // (Offset +2)
    uint8_t  File_logicalLo;    // (Offset +3)
    uint8_t  data[252];         // (Offset +4 to +255)
} SECTOR;

/* Done this way so it packs and is safe to access */
// Define the SIR structure 
typedef struct {
    uint8_t  volLabel[MAX_VOL_NAME_LEN]; // 00 - 10
    uint8_t  volNumberHi;                // 11 (can't use int Moto vs Intel byte order)
    uint8_t  volNumberLo;                // 12
    uint8_t  firstFreeTrack;             // 13
    uint8_t  firstFreeSector;            // 14
    uint8_t  lastFreeTrack;              // 15
    uint8_t  lastFreeSector;             // 16
    uint8_t  freeSectorsHi;              // 17
    uint8_t  freeSectorsLo;              // 18
    uint8_t  dateMonth;                  // 19
    uint8_t  dateDay;                    // 20
    uint8_t  dateYear;                   // 21
    uint8_t  endTrack;                   // 22
    uint8_t  endSector;                  // 23
} SIR_struct;

struct dir {
    char name[8];
    char ext[3];
    uint8_t pad0;
    uint8_t pad1;
    uint8_t strack;
    uint8_t ssec;
    uint8_t etrack;
    uint8_t esec;
    uint8_t sech;
    uint8_t secl;
    uint8_t rndf;	/* Only on Flex 6809 afaik */
    uint8_t pad2;
    uint8_t month;
    uint8_t day;
    uint8_t year;
};

// Directory Entry structure
typedef struct{
    char      fileName[8];     // 8 byte --- File name
    char      fileExt[3];      // 3 byte --- File extension
    uint8_t   unusedA;        // 2 byte --- Not used 
    uint8_t   unusedB;        // 2 byte --- Not used 
        
    uint8_t   startTrack;      // 1 byte --- Start track
    uint8_t   startSector;     // 1 byte --- Start sector
    uint8_t   endTrack;        // 1 byte --- End track
    uint8_t   endSector;       // 1 byte --- End sector

    uint8_t   totalSectorsHi;  // 2 byte --- Total number of sectors
    uint8_t   totalSectorsLo;  // 2 byte --- Total number of sectors

    uint8_t   randomFileFlag;  // 1 byte --- Random file flag (0xFF is Sequential/Text)

    uint8_t   unusedC;         // 1 byte --- Not used

    uint8_t   dateMonth;       // 1 byte --- Date month
    uint8_t   dateDay;         // 1 byte --- Date day
    uint8_t   dateYear;        // 1 byte --- Date year
} DIR_struct; // 24 bytes total
```
