:mod:`mozprocess` --- Launch and manage processes
=================================================

Mozprocess is a process-handling module that provides some additional
features beyond those available with python's subprocess:

* better handling of child processes, especially on Windows
* the ability to timeout the process after some absolute period, or some
  period without any data written to stdout/stderr
* the ability to specify output handlers that will be called
  for each line of output produced by the process
* the ability to specify handlers that will be called on process timeout
  and normal process termination

Caveat Emptor!
--------------
Most mozprocess development happened before Python 3; some key features of mozprocess were intended to work around bugs and inadequacies in Python subprocess which are no longer a concern today. Today we know that mozprocess has many bugs of its own, and the module has struggled to find modern ownership: THIS MODULE IS POORLY MAINTAINED.

At the same time, mozprocess has many clients and provides a convenient solution to some complex process handling needs. Our best advice:

* for routine process handling needs, use Python subprocess -- NOT mozprocess
* for convenient process handling with optional, simple output timeouts and/or output handling, use mozprocess.run_and_wait(); this is the simplest, most isolated, and most modern mozprocess interface
* for more complex needs, use mozprocess ProcessHandlerMixin/ProcessHandler but proceed with caution!

Running a process
-----------------

mozprocess consists of two classes: ProcessHandler inherits from ProcessHandlerMixin.

Let's see how to run a process.
First, the class should be instantiated with at least one argument which is a command (or a list formed by the command followed by its arguments).
Then the process can be launched using the *run()* method.
Finally the *wait()* method will wait until end of execution.

.. code-block:: python

    from mozprocess import processhandler

    # under Windows replace by command = ['dir', '/a']
    command = ['ls', '-l']
    p = processhandler.ProcessHandler(command)
    print("execute command: %s" % p.commandline)
    p.run()
    p.wait()

Note that using *ProcessHandler* instead of *ProcessHandlerMixin* will print the output of executed command. The attribute *commandline* provides the launched command.

Collecting process output
-------------------------

Let's now consider a basic shell script that will print numbers from 1 to 5 waiting 1 second between each.
This script will be used as a command to launch in further examples.

**proc_sleep_echo.sh**:

.. code-block:: sh

    #!/bin/sh

    for i in 1 2 3 4 5
    do
        echo $i
        sleep 1
    done

If you are running under Windows, you won't be able to use the previous script (unless using Cygwin).
So you'll use the following script:

**proc_sleep_echo.bat**:

.. code-block:: bat

    @echo off
    FOR %%A IN (1 2 3 4 5) DO (
        ECHO %%A
        REM if you have TIMEOUT then use it instead of PING
        REM TIMEOUT /T 1 /NOBREAK
        PING -n 2 127.0.0.1 > NUL
    )

Mozprocess allows the specification of custom output handlers to gather process output while running.
ProcessHandler will by default write all outputs on stdout. You can also provide (to ProcessHandler or ProcessHandlerMixin) a function or a list of functions that will be used as callbacks on each output line generated by the process.

In the following example the command's output will be stored in a file *output.log* and printed in stdout:

.. code-block:: python

    import sys
    from mozprocess import processhandler

    fd = open('output.log', 'w')

    def tostdout(line):
        sys.stdout.write("<%s>\n" % line)

    def tofile(line):
        fd.write("<%s>\n" % line)

    # under Windows you'll replace by 'proc_sleep_echo.bat'
    command = './proc_sleep_echo.sh'
    outputs = [tostdout, tofile]

    p = processhandler.ProcessHandlerMixin(command, processOutputLine=outputs)
    p.run()
    p.wait()

    fd.close()

The process output can be saved (*obj = ProcessHandler(..., storeOutput=True)*) so as it is possible to request it (*obj.output*) at any time. Note that the default value for *stroreOutput* is *True*, so it is not necessary to provide it in the parameters.

.. code-block:: python

    import time
    import sys
    from mozprocess import processhandler

    command = './proc_sleep_echo.sh' # Windows: 'proc_sleep_echo.bat'

    p = processhandler.ProcessHandler(command, storeOutput=True)
    p.run()
    for i in xrange(10):
        print(p.output)
        time.sleep(0.5)
    p.wait()

In previous example, you will see the *p.output* list growing.

Execution
---------

Status
``````

It is possible to query the status of the process via *poll()* that will return None if the process is still running, 0 if it ended without failures and a negative value if it was killed by a signal (Unix-only).

.. code-block:: python

    import time
    import signal
    from mozprocess import processhandler

    command = './proc_sleep_echo.sh'
    p = processhandler.ProcessHandler(command)
    p.run()
    time.sleep(2)
    print("poll status: %s" % p.poll())
    time.sleep(1)
    p.kill(signal.SIGKILL)
    print("poll status: %s" % p.poll())

Timeout
```````

A timeout can be provided to the *run()* method. If the process last more than timeout seconds, it will be stopped.

After execution, the property *timedOut* will be set to True if a timeout was reached.

It is also possible to provide functions (*obj = ProcessHandler(..., onTimeout=functions)*) that will be called if the timeout was reached.

.. code-block:: python

    from mozprocess import processhandler

    def ontimeout():
        print("REACHED TIMEOUT")

    command = './proc_sleep_echo.sh' # Windows: 'proc_sleep_echo.bat'
    functions = [ontimeout]
    p = processhandler.ProcessHandler(command, onTimeout=functions)
    p.run(timeout=2)
    p.wait()
    print("timedOut = %s" % p.timedOut)

By default the process will be killed on timeout but it is possible to prevent this by setting *kill_on_timeout* to *False*.

.. code-block:: python

    p = processhandler.ProcessHandler(command, onTimeout=functions, kill_on_timeout=False)
    p.run(timeout=2)
    p.wait()
    print("timedOut = %s" % p.timedOut)

In this case, no output will be available after the timeout, but the process will still be running.

Waiting
```````

It is possible to wait until the process exits as already seen with the method *wait()*, or until the end of a timeout if given. Note that in last case the process is still alive after the timeout.

.. code-block:: python

    command = './proc_sleep_echo.sh' # Windows: 'proc_sleep_echo.bat'
    p = processhandler.ProcessHandler(command)
    p.run()
    p.wait(timeout=2)
    print("timedOut = %s" % p.timedOut)
    p.wait()

Killing
```````

You can request to kill the process with the method *kill*. f the parameter "ignore_children" is set to False when the process handler class is initialized, all the process's children will be killed as well.

Except on Windows, you can specify the signal with which to kill method the process (e.g.: *kill(signal.SIGKILL)*).

.. code-block:: python

    import time
    from mozprocess import processhandler

    command = './proc_sleep_echo.sh' # Windows: 'proc_sleep_echo.bat'
    p = processhandler.ProcessHandler(command)
    p.run()
    time.sleep(2)
    p.kill()

End of execution
````````````````

You can provide a function or a list of functions to call at the end of the process using the initialization parameter *onFinish*.

.. code-block:: python

    from mozprocess import processhandler

    def finish():
        print("Finished!!")

    command = './proc_sleep_echo.sh' # Windows: 'proc_sleep_echo.bat'

    p = processhandler.ProcessHandler(command, onFinish=finish)
    p.run()
    p.wait()

Child management
----------------

Consider the following scripts:

**proc_child.sh**:

.. code-block:: sh

    #!/bin/sh
    for i in a b c d e
    do
        echo $i
        sleep 1
    done

**proc_parent.sh**:

.. code-block:: sh

    #!/bin/sh
    ./proc_child.sh
    for i in 1 2 3 4 5
    do
        echo $i
        sleep 1
    done

For windows users consider:

**proc_child.bat**:

.. code-block:: bat

    @echo off
    FOR %%A IN (a b c d e) DO (
        ECHO %%A
        REM TIMEOUT /T 1 /NOBREAK
        PING -n 2 127.0.0.1 > NUL
    )

**proc_parent.bat**:

.. code-block:: bat

    @echo off
    call proc_child.bat
    FOR %%A IN (1 2 3 4 5) DO (
        ECHO %%A
        REM TIMEOUT /T 1 /NOBREAK
        PING -n 2 127.0.0.1 > NUL
    )

For processes that launch other processes, mozprocess allows you to get child running status, wait for child termination, and kill children.

Ignoring children
`````````````````

By default the *ignore_children* option is False. In that case, killing the main process will kill all its children at the same time.

.. code-block:: python

    import time
    from mozprocess import processhandler

    def finish():
        print("Finished")

    command = './proc_parent.sh'
    p = processhandler.ProcessHandler(command, ignore_children=False, onFinish=finish)
    p.run()
    time.sleep(2)
    print("kill")
    p.kill()

If *ignore_children* is set to *True*, killing will apply only to the main process that will wait children end of execution before stopping (join).

.. code-block:: python

    import time
    from mozprocess import processhandler

    def finish():
        print("Finished")

    command = './proc_parent.sh'
    p = processhandler.ProcessHandler(command, ignore_children=True, onFinish=finish)
    p.run()
    time.sleep(2)
    print("kill")
    p.kill()

API Documentation
-----------------

.. module:: mozprocess

  .. automethod:: mozprocess.run_and_wait

  .. autoclass:: ProcessHandlerMixin
     :members: __init__, timedOut, commandline, run, kill, processOutputLine, onTimeout, onFinish, wait

  .. autoclass:: ProcessHandler
     :members:
