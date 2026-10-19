# flexnet

## fcreate.asm

## fdrive.asm

## fexit.asm

## fmount.asm

## fnetdrv.asm

## memdump.asm

## RCD Remote Change Directory

        This command will change the current directory,  which is the default
        directory which the other utilities will use. For example, if no file
        path is provided in the RCREATE command, the new file will be created
        in the current directory.

        Syntax: RCD <filepath>

        Example: RCD C:\folder3

## RDIR    Remote Directory

        RDIR  performs a DIR command under DOS,  and returns an exact copy of
        the listing generated on the MS-DOS screen:
        - The current directory is used
        - Only files with the extension .DSK will be listed.
        - Command  line  parameters  are allowed,  and are passed  to the DOS
        command.
        - RDIR  displays  20 lines  at a time,  then prompts  the user before
        displaying  the next 20 lines.  Typing a space will  display the next
        lines,  typing [esc]  will stop  the listing  and return  to the Flex
        prompt.

        Examples
        +++RDIR [cr]          Lists all DSK files in the current directory

        +++RDIR A2* [cr]      Lists all DSK files which start with A2.

## RDRIVE  Change drive

        RDRIVE instructs  the MS-DOS program to point to  another drive, thus
        offering the ability to access any DSK file on any drive.

        Syntax: RDRIVE [letter]

        If no  drive  letter is  specified,  RDRIVE will  display the current
        drive and directory path.

## RESYNC  Re-SYNChronize

        If something should happen and you restart the PC, it is looking  for
        the Sync information  from the FLEX system.  RESYNC will re-establish
        the  communication link  and you can proceed.  The command is simply:
        RESYNC. If all is working properly you should not have to do anything
        at the PC end of the link.

        Syntax: RESYNC [cr]

## REXIT   Remote Exit

        This command will close all files open on the PC and exit to DOS.  On
        the Flex system,  it will uninstall  the NETDRV drivers:  restore the
        Flex drivers  to their  original values  (i.e. what they  were before
        NETDRV was run),  clear the memory space used  by  the NETPC drivers,
        and restore MEMEND to make the memory space available again for other
        programs.

        Note: MEMEND will be changed ONLY if no other program has changed  it
              after NETDRV was loaded;  in other words,  memory will be freed
              only if  the NETDRV code is right  "above" MEMEND;  see section
              6.3 for more detail.

## RLIST   Remote List

        RLIST display  a list  of all the directories  which are contained in
        the current  directory.  It is  provided  as an aid  to the user when
        "navigating" in the MS-DOS  directory tree structure.  No  parameters
        are needed.  RLIST displays the listing  in the same fashion as RDIR,
        i.e. 20 lines at a time, then prompting the user for more.

        Syntax: RLIST [cr]

## RCREATE Remote Create

        The RCREATE command will prompt the user for the location,  name, and
        other parameters to create a new DSK file.  After all parameters  are
        entered,  the user will be asked for a confirmation  (yes/no)  and if
        the answer is positive the file will be created.
        No parameters are required in the command line.

        Example:
        +++RCREATE
        File path ? C:\folder3
        File name ? newname
        Disk number (in decimal) ? 123
        Number of tracks (in decimal) ? 35
        Sectors per track (in decimal) ? 10
        OK to proceed ? Y

        File successfully created
        +++


        NOTES:
        - The file path  is the directory where the new file will be created.
        If none  is  specified,   the  file  will be  created  in the current
        directory.
        - The  file name  ("newname")  will be used for both the  name of the
        DSK file, as known by MS-DOS, and for the volume name which is stored
        in the SIR (track 0, sector 3) of the DSK file.
        - The  disk number,  123  in this example,  is  stored  as the volume
        number in the SIR.
        - If non-numeric  values are entered for the disk number,  the number
        of tracks or  the number  of sectors per track,  the question will be
        repeated.
        - RCREATE  does not  check  for the  "validity"  of the  track/sector
        format entered by the user.  The file will be created, as long as the
        values given are numeric.
        - In the above example, the disk image will be created with 35 tracks
        (including track 0) of 10 sectors, for a total of 340 data sectors.
        - Note that  RCREATing  a disk  does  not  automatically RMOUNT it or
        change the current directory.  You must use RCD to change directories
        if needed, and RMOUNT to mount the new disk.

## RDELETE Remote Delete

        RDELETE will  prompt the  user for the file path and  the name of the
        file to be  deleted. If no file path is given,  the current directory
        will be assumed.  RDELETE also adds a .DSK extension to the filename.

        RDELETE  will then  display the  full path and  name of the file, and
        will  prompt the  user for  a response  (Yes/No)  before deleting the
        file.

        If the file cannot be found, an error message will be displayed.

        Syntax: RDELETE <filename> [cr]

## RMOUNT  Remote Mount

        RMOUNT will "mount" a DSK file to drive 3,  in other words it selects
        a DSK file,  opens it and  sets it as Drive #3. After a file has been
        RMOUNTed,  it can be read or written to by Flex  as drive #3 as if it
        were a standard floppy drive.  The user  may specify just the name of
        the file,  in which  case RMOUNT assumes  the file  is in the current
        directory.  Otherwise,  the user may type  the complete path and file
        name to RMOUNT a file which is in another directory.
        RMOUNT does not require a file extension to be typed; however, if one
        is entered, it must be .DSK (lowecase letters OK) or an error message
        will be generated.

        NOTE: There is  no need to  "unmount" a file before RMOUNTing another
              one.  However, a file which is "mounted" is  open under MS-DOS,
              and may be corrupted if  the PC is powered  down while the file
              is open.


        Examples:
        +++RMOUNT <filename.dsk> [cr]
        +++RMOUNT <filename> [cr]      Mount a  file which is  in the current
                                       directory.   NETPC  will automatically
                                       add  the .DSK  extension  to  the file
                                       name.

        +++RMOUNT c:\folder\filename [cr]
                                       Mount   a   file   which   is   in any
                                       directory.  In this case the file will
                                       be   mounted   without   changing  the
                                       current directory.

## 
