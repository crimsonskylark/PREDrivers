### Practical Reverse Engineering

This driver implements my solution to one of the exercises in chapter 3 of the book "Practical Reverse Engineering".

Problem statement:

>Write a driver that enumerates all user-mode and kernel-mode APCs for
all threads in a process. Hint: You need to take into consideration IRQL
level when performing the enumeration.

### Explanation

This solution starts by creating a system where where all the computations will be performed. Once this new thread starts execution, we get a pointer to the opaque `EPROCESS` structure, which represents the process in memory.

In kernel mode, the first member of this structure is `KPROCESS`, is what we're actually interested in for the purposes of this exercise. Although not available in code (or necessarily stable across different versions) both of these structures can be investigated in WinDbg when Microsoft debug symbols are available using the `dt` command, however, posting them in their entirety here would be unproductive as they are both fairly large structures, so only relevant members will be present in this article. 

The field we're interested in is `ThreadListHead` which as of writing this post is at offset `0x30`.

```
4: kd> dt nt!_KPROCESS
	...
   +0x030 ThreadListHead   : _LIST_ENTRY
	...
```

By traversing this list, you can get a pointer to every thread associated with this process. A thread is represented in memory by a `KTHREAD` object, and at offset `0x2f8` we have the `ThreadListEntry` field, which is what gets added to the list at `KPROCESS.ThreadListHead` once a new thread is created by the system. By subtracting `0x2f8` from the list entry pointer we get the start of the `KTHREAD` that holds the `ThreadListEntry`.  Note that if you had the structure type defined in your code the `CONTAINING_RECORD` macro could be used instead.

```cpp
auto proc = PsGetCurrentProcess();
auto procThreadListHead = reinterpret_cast<LIST_ENTRY*>(reinterpret_cast<CHAR*>(proc) + THREAD_LIST_HEAD); // THREAD_LIST_HEAD = 0x30
    
for (auto Entry = procThreadListHead->Flink; Entry != procThreadListHead; Entry = Entry->Flink) {
    auto threadObj = reinterpret_cast<CHAR*>(Entry) - THREAD_LIST_ENTRY; // THREAD_LIST_ENTRY = 0x2f8
    ...
}
```


With a pointer to the `KTHREAD` in hand, we want to get to the `ApcState` (offset `0x98`) field, which holds two lists of APCs queued to be executed in the thread: one for kernel-mode APCs and the other for user-mode APCs, respectively. Now, it's simply a matter of getting a pointer to one (or both) lists, iterate through it the same way we did above and perform some arithmetic to get the `PKAPC` object in question.