This is a tool for extracting and building 3DS darc archive files. In some cases the file extension is ".arc". Remember, if the archive is compressed you must use another tool to handle that. Also note that when building archives, it goes by the order returned by readdir(), hence the order in the archive in some cases may not match the original official archive which was extracted previously. Due to this and alignment, the built archive filesize may not match the original official archive filesize either, but this doesn't actually matter.  
The utils.* and types.h files are from ctrtool.

