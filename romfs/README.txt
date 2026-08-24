Poppins-Regular.bcfnt and Poppins-SemiBold.bcfnt are baked into the
project -- nothing to convert for a normal build.

Only regenerate these if you want to change font/size (currently both
converted at size 12) or swap in a different typeface:

    mkbcfnt -s 12 -o romfs/Poppins-Regular.bcfnt  Poppins-Regular.ttf
    mkbcfnt -s 12 -o romfs/Poppins-SemiBold.bcfnt Poppins-SemiBold.ttf
