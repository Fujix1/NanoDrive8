Option Explicit
Dim shell, files, script, command
Set shell = CreateObject("WScript.Shell")
Set files = CreateObject("Scripting.FileSystemObject")
script = files.BuildPath(files.GetParentFolderName(WScript.ScriptFullName), "NanoDriveBridge.pyw")
command = "pythonw.exe """ & script & """"
On Error Resume Next
shell.Run command, 1, False
If Err.Number <> 0 Then
    MsgBox "Python was not found. Install Python and make pythonw.exe available in PATH.", vbExclamation, "NanoDrive MAME Bridge"
End If
