# dsk2woz2
Convert Apple II 5.25" DSK disk images into writeable [WOZ](https://applesaucefdc.com/woz/reference2/) 2.0 (Applesauce) disk images

This is a portable C-only one-way converter from 16-sector DSK images to the latest spec (2.0) WOZ files, including the newer embedded write instructions to allow creating a physical copy using [Applesauce](https://applesaucefdc.com). No metadata is embedded in the resulting image.

### How to build and run 
It's all in one file and there are no dependencies beyond the C standard libs. So go ahead and:

    cc dsk2woz2.c -o dsk2woz2

Then:

    ./dsk2woz2 input.dsk output.woz

Reads the contents of the disk `input.dsk` and outputs the file `output.woz`.

### When do I need this?

The equivalent conversion functionality is built into Applesauce itself (open a DSK file, then export to WOZ), so honestly, you probably don't need it. I wrote it as a learning exploration. 

That said, if you aren't running on a Mac, or need to batch automate from the command line, dsk2woz2 may help you. It will produce an identical WOZ image as doing the load-and-export steps with the Applesauce application.

## DOS 3.3 vs ProDOS
Apple II DSK images are typically *stored* in DOS 3.3 sector order-- even for disks which contain ProDOS volumes. *Some* ProDOS disk images are stored in the ProDOS native sector order; these usually have the file extension `.po`. The tool will automatically use ProDOS sectors if the input file has a `.po` extension, otherwise it will use DOS 3.3 order. If this explanation gibberish to you, don't worry about it. The default should be fine.

### Thanks

Parts of dsk2woz2 were cribbed and/or derived from [dsk2woz](https://github.com/TomHarte/dsk2woz) by Tom Harte. (That program was built to generate WOZ 1.0 files.) In particular, the 6-and-2 encoding and track to nibble stream guts. Thanks to Tom. 

Thanks also, of course, to John K. Morris, the prime mover behind the Applesauce project and a graciously helpful fellow. 
