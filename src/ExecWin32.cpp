/* exec-win32.c -- Windows subprocess handling
 *
 * This module is complicated due to how Win32 handles child I/O and because
 * its command processor cannot handle long command lines, requiring tools to
 * support so-called "response files" which are basically just the contents of
 * the command line, but written to a file. Tundra implements this via the
 * @RESPONSE handling.
 *
 * Also, rather than using the tty merging module that unix uses, this module
 * handles output merging via temporary files.  This removes the pipe
 * read/write deadlocks I've seen from time to time when using the TTY merging
 * code. Rather than losing my last sanity points on debugging win32 internals
 * I've opted for this much simpler approach.
 *
 * Instead of buffering stuff in memory via pipe babysitting, this new code
 * first passes the stdout handle straight down to the first process it spawns.
 * Subsequent child processes that are spawned when the TTY is busy instead get
 * to inherit a temporary file handle. Once the process completes, it will wait
 * to get the TTY and flush its temporary output file to the console. The
 * temporary is then deleted.
 */

#include "Exec.hpp"
#include "Common.hpp"
#include "Config.hpp"
#include "Mutex.hpp"
#include "BuildQueue.hpp"
#include "Atomic.hpp"

#include <algorithm>

#if defined(TUNDRA_WIN32)

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

namespace t2
{

static char              s_TemporaryDir[MAX_PATH];
static DWORD             s_TundraPid;
static Mutex             s_FdMutex;

static HANDLE s_TempFiles[kMaxBuildThreads];

static HANDLE AllocFd(int job_id)
{
  HANDLE result = s_TempFiles[job_id];

  if (!result)
  {
    char temp_dir[MAX_PATH + 1];
    DWORD access, sharemode, disp, flags;

    _snprintf(temp_dir, MAX_PATH, "%stundra.%u.%d", s_TemporaryDir, s_TundraPid, job_id);
    temp_dir[MAX_PATH] = '\0';

    access    = GENERIC_WRITE | GENERIC_READ;
    sharemode = FILE_SHARE_WRITE;
    disp      = CREATE_ALWAYS;
    flags     = FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE;

    result = CreateFileA(temp_dir, access, sharemode, NULL, disp, flags, NULL);

    if (INVALID_HANDLE_VALUE == result)
    {
      fprintf(stderr, "failed to create temporary file %s\n", temp_dir);
      return INVALID_HANDLE_VALUE;
    }

    SetHandleInformation(result, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    s_TempFiles[job_id] = result;
  }

  return result;
}

static void FreeFd(int job_id, HANDLE h)
{
  char buf[1024];
  HANDLE target = GetStdHandle(STD_OUTPUT_HANDLE);

  DWORD fsize = SetFilePointer(h, 0, NULL, FILE_CURRENT);

  // Rewind file position of the temporary file.
  SetFilePointer(h, 0, NULL, FILE_BEGIN);

  // Wait until we can take the TTY.
  MutexLock(&s_FdMutex);
  
  // Win32-specific hack to get rid of "Foo.cpp" output from visual studio's cl.exe
  if (fsize < sizeof(buf) - 1)
  {
    DWORD rb;
    if (ReadFile(h, buf, sizeof(buf) - 1, &rb, NULL))
    {
      buf[rb] = '\0';
      static const struct { const char* text; size_t len; } exts[] =
      {
        { ".cpp\r\n", 6 },
        { ".c++\r\n", 6 },
        { ".c\r\n",   4 },
        { ".CC\r\n",  5 }
      };

      int ext_index = -1;
      for (size_t i = 0; -1 == ext_index && i < ARRAY_SIZE(exts); ++i)
      {
        if (rb > exts[i].len && 0 == memcmp(buf + rb - exts[i].len, exts[i].text, exts[i].len))
        {
          ext_index = (int) i;
          break;
        }
      }

      bool is_src_file = false;
      if (ext_index >= 0)
      {
        is_src_file = true;
        for (DWORD i = 0; i < rb - exts[ext_index].len; ++i)
        {
          if (!isalnum(buf[i]))
          {
            is_src_file = false;
            break;
          }
        }
      }

      if (!is_src_file)
      {
        DWORD wb;
        WriteFile(target, buf, rb, &wb, NULL);
      }
      else
      {
        // Be silent
      }
    }
  }
  else
  {
    // Dump the contents of the temporary file to the TTY.
    for (;;)
    {
      DWORD rb, wb;
      if (!ReadFile(h, buf, sizeof(buf), &rb, NULL) || rb == 0)
        break;
      if (!WriteFile(target, buf, rb, &wb, NULL))
        break;
    }
  }

  // Mark the TTY as free again and wake waiters.
  MutexUnlock(&s_FdMutex);

  // Truncate the temporary file for reuse
  SetFilePointer(h, 0, NULL, FILE_BEGIN);
  SetEndOfFile(h);
}

static struct Win32EnvBinding
{
  const char* m_Name;
  const char* m_Value;
  size_t      m_NameLength;
} g_Win32Env[1024];

static size_t g_Win32EnvCount;

void ExecInit(void)
{
  s_TundraPid = GetCurrentProcessId();

  if (0 == GetTempPathA(sizeof(s_TemporaryDir), s_TemporaryDir))
    Croak("error: couldn't get temporary directory path");

  MutexInit(&s_FdMutex);

  // Initialize win32 env block. We're going to let it leak.
  // This block contains a series of nul-terminated strings, with a double nul
  // terminator to indicated the end of the data.
  const char* env = GetEnvironmentStringsA();
  size_t env_count = 0;

  while (*env && env_count < ARRAY_SIZE(g_Win32Env))
  {
    size_t len = strlen(env);

    if (const char* eq = strchr(env, '='))
    {
      // Discard empty variables that Windows sometimes has internally
      if (eq != env)
      {
        g_Win32Env[env_count].m_Name = env;
        g_Win32Env[env_count].m_Value = eq+1;
        g_Win32Env[env_count].m_NameLength = size_t(eq - env);
        ++env_count;
      }
    }

    env += len + 1;
  }

  g_Win32EnvCount = env_count;
}

static bool
AppendEnvVar(char* block, size_t block_size, size_t *cursor, const char *name, size_t name_len, const char* value)
{
  size_t value_len = strlen(value);
  size_t pos       = *cursor;

  if (pos + name_len + value_len + 2 > block_size)
    return false;

  memcpy(block + pos, name, name_len);
  pos += name_len;

  block[pos++] = '=';
  memcpy(block + pos, value, value_len);
  pos += value_len;

  block[pos++] = '\0';

  *cursor = pos;
  return true;
}

extern char* s_Win32EnvBlock;

static bool
MakeEnvBlock(char* env_block, size_t block_size, const EnvVariable *env_vars, int env_count)
{
  size_t cursor = 0;
  size_t env_var_len[ARRAY_SIZE(g_Win32Env)];
  unsigned char used_env[ARRAY_SIZE(g_Win32Env)];

  if (env_count > int(sizeof used_env))
    return false;

  for (int i = 0; i < env_count; ++i)
  {
    env_var_len[i] = strlen(env_vars[i].m_Name);
  }

  memset(used_env, 0, sizeof(used_env));

  // Loop through windows environment block and emit anything we're not going to override.
  for (size_t i = 0, count = g_Win32EnvCount; i < count; ++i)
  {
    bool replaced = false;

    for (int x = 0; !replaced && x < env_count; ++x)
    {
      if (used_env[x])
        continue;

      size_t len = env_var_len[x];
      if (len == g_Win32Env[i].m_NameLength && 0 == _memicmp(g_Win32Env[i].m_Name, env_vars[x].m_Name, len))
      {
        if (!AppendEnvVar(env_block, block_size, &cursor, env_vars[x].m_Name, len, env_vars[x].m_Value))
          return false;
        replaced = true;
        used_env[x] = 1;
      }
    }

    if (!replaced)
    {
      if (!AppendEnvVar(env_block, block_size, &cursor, g_Win32Env[i].m_Name, g_Win32Env[i].m_NameLength, g_Win32Env[i].m_Value))
        return false;
    }
  }

  // Loop through our env vars and emit those we didn't already push out.
  for (int i = 0; i < env_count; ++i)
  {
    if (used_env[i])
      continue;
    if (!AppendEnvVar(env_block, block_size, &cursor, env_vars[i].m_Name, env_var_len[i], env_vars[i].m_Value))
      return false;
  }

  env_block[cursor] = '\0';
  env_block[cursor+1] = '\0';
  return true;
}

static void
PrintLineToHandle(HANDLE h, const char *str)
{
  DWORD bw;
  WriteFile(h, str, (DWORD) strlen(str), &bw, NULL);
  WriteFile(h, "\r\n", 2, &bw, NULL);
}

/*
   Win32Spawn -- spawn and wait for a a sub-process on Windows.

   We would like to say:

    return (int) _spawnlpe(_P_WAIT, "cmd", "/c", cmd_line, NULL, env);

   but it turns out spawnlpe() isn't thread-safe (MSVC2008) when setting
   environment data!  So we're doing it the hard way. It also would set the
   environment in the right way as we want to define our environment strings in
   addition to whatever is already set, not replace everything.
 */

static int Win32Spawn(int job_id, const char *cmd_line, const EnvVariable *env_vars, int env_count, const char *annotation, const char* echo_cmdline)
{
  int result_code = 1;
  char buffer[8192];
  char env_block[128*1024];
  HANDLE output_handle;
  STARTUPINFO sinfo;
  PROCESS_INFORMATION pinfo;

  MutexLock(&s_FdMutex);
  {
    HANDLE target = GetStdHandle(STD_OUTPUT_HANDLE);
    const char crlf[] = "\r\n";
    DWORD wb;
    if (annotation)
    {
      WriteFile(target, annotation, (DWORD) strlen(annotation), &wb, NULL);
      WriteFile(target, crlf, 2, &wb, NULL);
    }
    if (echo_cmdline)
    {
      WriteFile(target, echo_cmdline, (DWORD) strlen(echo_cmdline), &wb, NULL);
      WriteFile(target, crlf, 2, &wb, NULL);
    }
  }
  MutexUnlock(&s_FdMutex);


  if (!MakeEnvBlock(env_block, sizeof(env_block) - 2, env_vars, env_count))
  {
    fprintf(stderr, "%d: env block error; too big?\n", job_id);
    return 1;
  }

  _snprintf(buffer, sizeof(buffer), "cmd.exe /c \"%s\"", cmd_line);
  buffer[sizeof(buffer)-1] = '\0';

  output_handle = AllocFd(job_id);

  memset(&pinfo, 0, sizeof(pinfo));
  memset(&sinfo, 0, sizeof(sinfo));
  sinfo.cb = sizeof(sinfo);
  sinfo.hStdInput = NULL;
  sinfo.hStdOutput = sinfo.hStdError = output_handle;
  sinfo.dwFlags = STARTF_USESTDHANDLES;

  if (CreateProcessA(NULL, buffer, NULL, NULL, TRUE, 0, env_block, NULL, &sinfo, &pinfo))
  {
    DWORD result;
    CloseHandle(pinfo.hThread);

    while (WAIT_OBJECT_0 != WaitForSingleObject(pinfo.hProcess, INFINITE))
      /* nop */;

    GetExitCodeProcess(pinfo.hProcess, &result);
    CloseHandle(pinfo.hProcess);
    result_code = (int) result;
  }
  else
  {
    char error[256];
    _snprintf(error, sizeof error, "ERROR: Couldn't launch process. Win32 error = %d", (int) GetLastError());
    PrintLineToHandle(output_handle, error);
  }

  FreeFd(job_id, output_handle);

  return result_code;
}

ExecResult ExecuteProcess(
      const char*         cmd_line,
      int                 env_count,
      const EnvVariable*  env_vars,
      int                 job_id,
      int                 echo_cmdline,
      const char*         annotation)
{
  static const char response_prefix[] = "@RESPONSE|";
  static const size_t response_prefix_len = sizeof(response_prefix) - 1;
  char command_buf[512];
  char option_buf[32];
  char new_cmd[8192];
  const char* response;

  ExecResult result;
  result.m_WasSignalled = false;
  result.m_ReturnCode   = 1;

  /* scan for a @RESPONSE|<option>|.... section at the end of the command line */
  if (NULL != (response = strstr(cmd_line, response_prefix)))
  {
    const size_t cmd_len = strlen(cmd_line);
    const char *option, *option_end;

    option = response + response_prefix_len;

    if (NULL == (option_end = strchr(option, '|')))
    {
      fprintf(stderr, "badly formatted @RESPONSE section; no comma after option: %s\n", cmd_line);
      return result;
    }

    /* Limit on XP and later is 8191 chars; but play it safe */
    if (cmd_len > 8000)
    {
      char tmp_dir[MAX_PATH];
      char response_file[MAX_PATH];
      int cmd_result;
      DWORD rc;

      rc = GetTempPath(sizeof(tmp_dir), tmp_dir);
      if (rc >= sizeof(tmp_dir) || 0 == rc)
      {
        fprintf(stderr, "couldn't get temporary directory for response file; win32 error=%d", (int) GetLastError());
        return result;
      }

      if ('\\' == tmp_dir[rc-1])
        tmp_dir[rc-1] = '\0';

      static uint32_t foo = 0;
      uint32_t sequence = AtomicIncrement(&foo);

      _snprintf(response_file, sizeof response_file, "%s\\tundra.resp.%u.%u", tmp_dir, GetCurrentProcessId(), sequence);
      response_file[sizeof(response_file)-1] = '\0';

      {
        HANDLE hf = CreateFileA(response_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (INVALID_HANDLE_VALUE == hf)
        {
          fprintf(stderr, "couldn't create response file %s; @err=%u", response_file, (unsigned int) GetLastError());
          return result;
        }

        DWORD written;
        WriteFile(hf, option_end + 1, (DWORD) strlen(option_end + 1), &written, NULL);

        if (!CloseHandle(hf))
        {
          fprintf(stderr, "couldn't close response file %s: errno=%d", response_file, errno);
          return result;
        }
        hf = NULL;
      }

      {
        int copy_len = std::min((int) (response - cmd_line), (int) (sizeof(command_buf) - 1));
        strncpy(command_buf, cmd_line, copy_len);
        command_buf[copy_len] = '\0';
        copy_len = std::min((int) (option_end - option), (int) (sizeof(option_buf) - 1));
        strncpy(option_buf, option, copy_len);
        option_buf[copy_len] = '\0';
      }

      _snprintf(new_cmd, sizeof(new_cmd), "%s %s%s", command_buf, option_buf, response_file);
      new_cmd[sizeof(new_cmd)-1] = '\0';

      cmd_result = Win32Spawn(job_id, new_cmd, env_vars, env_count, annotation, echo_cmdline ? cmd_line : NULL);

      remove(response_file);
      result.m_ReturnCode = cmd_result;
      return result;
    }
    else
    {
      size_t i, len;
      int copy_len = std::min((int) (response - cmd_line), (int) (sizeof(command_buf) - 1));
      strncpy(command_buf, cmd_line, copy_len);
      command_buf[copy_len] = '\0';
      _snprintf(new_cmd, sizeof(new_cmd), "%s%s", command_buf, option_end + 1);
      new_cmd[sizeof(new_cmd)-1] = '\0';

      /* Drop any newlines in the command line. They are needed for response
       * files only to make sure the max length doesn't exceed 128 kb */
      for (i = 0, len = strlen(new_cmd); i < len; ++i)
      {
        if (new_cmd[i] == '\n')
        {
          new_cmd[i] = ' ';
        }
      }

      result.m_ReturnCode = Win32Spawn(job_id, new_cmd, env_vars, env_count, annotation, echo_cmdline ? cmd_line : NULL);
      return result;
    }
  }
  else
  {
    size_t i, len;

    /* Drop any newlines in the command line. They are needed for response
     * files only to make sure the max length doesn't exceed 128 kb */
    strncpy(new_cmd, cmd_line, sizeof(new_cmd));
    new_cmd[sizeof(new_cmd)-1] = '\0';

    for (i = 0, len = strlen(new_cmd); i < len; ++i)
    {
      if (new_cmd[i] == '\n')
      {
        new_cmd[i] = ' ';
      }
    }

    result.m_ReturnCode = Win32Spawn(job_id, new_cmd, env_vars, env_count, annotation, echo_cmdline ? cmd_line : NULL);
    return result;
  }
}

}

#endif /* TUNDRA_WIN32 */