genmsg.exe netmsg.xml Struct_h.tpl Struct.h
copy Struct.h ..\..\common\NetMsg.h /Y

genmsg.exe servermsg.xml Struct_h.tpl Struct.h
copy Struct.h ..\..\common\ServerNetMsg.h /Y

genmsg.exe netmsg.xml Struct_fk.tpl Struct.fk
copy Struct.fk ..\..\bin\script\NetMsg.fk /Y

genmsg.exe servermsg.xml Struct_fk.tpl Struct.fk
copy Struct.fk ..\..\bin\script\ServerNetMsg.fk /Y

genmsg.exe netmsg.xml Struct_h_client.tpl StructClient.h
copy StructClient.h ..\..\..\client\client\sdk\net\include\NetMsg.h /Y
