// Exercises enough of the C library (threads, files, time, `printf`, the maths routines) to tell
// whether a program which got linked against one of the bundled libcs works, see `run.sh`.
package test_cross

import "core:fmt"
import "core:os"
import "core:thread"
import "core:sync"
import "core:time"
import "core:c/libc"
import "core:math"
import "core:strconv"
import "core:slice"
import "core:mem"

counter: int
mutex: sync.Mutex

worker :: proc(t: ^thread.Thread) {
	for _ in 0..<1000 {
		sync.lock(&mutex)
		counter += 1
		sync.unlock(&mutex)
	}
}

main :: proc() {
	threads: [4]^thread.Thread
	for &t in threads {
		t = thread.create(worker)
		thread.start(t)
	}
	for t in threads {
		thread.join(t)
		thread.destroy(t)
	}
	fmt.println("counter:", counter)

	libc.printf("libc printf: %d %s %.3f\n", libc.int(42), cstring("str"), f64(3.14159))
	libc.fflush(libc.stdout)

	start := time.now()
	time.sleep(2 * time.Millisecond)
	fmt.println("slept >= 2ms:", time.since(start) >= 2*time.Millisecond)

	path := "cross_test_tmp.txt"
	werr := os.write_entire_file(path, "hello file\n")
	data, rerr := os.read_entire_file(path, context.allocator)
	fmt.printf("file: %q %v %v\n", string(data), werr, rerr)
	os.remove(path)

	v, _ := strconv.parse_f64("2.5")
	fmt.println("pow:", math.pow(v, 3), "sin:", math.sin(f64(1)))

	xs := []int{5, 2, 9, 1}
	slice.sort(xs)
	fmt.println(xs)

	tracking: mem.Tracking_Allocator
	mem.tracking_allocator_init(&tracking, context.allocator)
	context.allocator = mem.tracking_allocator(&tracking)
	p := new(int)
	free(p)
	fmt.println("leaks:", len(tracking.allocation_map))
	fmt.println("os/arch:", ODIN_OS, ODIN_ARCH, "now year >= 2024:", time.year(time.now()) >= 2024)
}
