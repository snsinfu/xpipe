xpipe
=====

`xpipe` splits stdin into chunks of lines and pipes each chunk to other command.

## Usage

    Usage: xpipe [-h] [-b bufsize] [-t timeout] command ...

    Options
      -b bufsize  set buffer size in bytes
      -t timeout  set read timeout in seconds
      -h          show this help

`command ...` is executed for each block of lines. The `-b bufsize` option sets
the maximum size of a block.

### Example

Suppose you need to post sensor metric data to a REST API endpoint. And to
reduce overhead, the metric data should be sent in batches.

Then, you just write a `sensor` program that reads metric data and writes each
sample point to stdout line-by-line (here data is formatted in CSV):

    $ sensor
    2017-12-08 00:00:00, 1.23
    2017-12-08 00:00:01, 2.34
    2017-12-08 00:00:09, 3.45
    ...

Pipe it to `xpipe curl` and you are done!

    $ sensor | xpipe curl -X POST -H "Content-Type: text/csv" -d @- http://example.com/metric

`curl` requests are automatically done in batches thanks to `xpipe`. If you
want to flush batch after 10 seconds of no data, use `-t 10` option:

    $ sensor | xpipe -t 10 curl -X POST -H "Content-Type: "text/csv" -d @- http://example.com/metric

## Installation

Build and install:

    make
    cp xpipe ~/bin

Requires POSIX environemnt with C99 compiler.

## Test

    make test

Some tests take a few seconds for testing the timeout functionality.

## License

MIT
