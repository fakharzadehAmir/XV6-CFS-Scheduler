#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

/*

There is a data structure which is called Red-Black tree structure.
Red-Black Tree: 
  A red-black tree is a binary search tree which has the following red-black properties:
    Every node is either red or black. Every leaf (NULL) is black.
    If a node is red, then both its children are black.

*/ 

struct RedBlack_Tree{
  struct spinlock lock;
  struct proc *root, *min_vruntime;
  int period, count ,weight;
} redBlackTree;

static struct RedBlack_Tree *tasks = &redBlackTree;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

static int latency = NPROC / 2, min_gran = 2;
// redblackinit
void rbTree_init(struct RedBlack_Tree *rb_tree, char *lock_name) {
  rb_tree->period = latency;
  rb_tree->root = 0;
  rb_tree->min_vruntime = 0;
  rb_tree->weight = 0;
  rb_tree->count = 0;
  initlock(&rb_tree->lock, lock_name);
}
// isEmpty
int isEmpty(struct RedBlack_Tree *tree) { tree->count == 0; }

// isFull
int isFull(struct RedBlack_Tree *tree) { tree->count == NPROC; }

// calculateWeight
/*
  computeProcessWeight(int)
  parameters: processNiceValue - the nice value of the process
  returns: an integer that signifies the weight of the process.
  This function calculates the weight of a process based on its nice value.

  Note: In the Linux kernel, the nice value can range from -20 to 19. However, for our xv6 implementation, we will use the range 0 to 30.
  The default nice value for a process is set to 0.

  The formula to determine the weight of a process is:
  1024/(1.25 ^ nice value of process)
*/
int
computeProcessWeight(int processNiceValue){

  // The denominator for the weight calculation formula
  double weightDenominator = 1.25;

  // If a process has a nice value greater than 30, it is set to 30.
  // This is to ensure that the priority level is accurately utilized without losing precision due to the fraction being cast to an int.
  if(processNiceValue > 30){
	processNiceValue = 30;
  }
  
  // This loop calculates (1.25 ^ nice value) which is used as the denominator in the formula to find the weight. 
  int counter = 0;
  while (counter < processNiceValue && processNiceValue > 0){
  	// Multiply the current denominator by 1.25 for each increment of the nice value
  	weightDenominator = weightDenominator * 1.25;
  }

  // The weight of the process is calculated as 1024 divided by the weight denominator.
  // The result is cast to an int to ensure that the function returns an integer value.
  return (int) (1024/weightDenominator);
}


// rotateLeft
void rotateLeft(struct RedBlack_Tree *tree, struct proc *positonProc) {
  struct proc *right_proc = positonProc->proc_right;
  
  positonProc->proc_right = right_proc->proc_left;
  if (right_proc->proc_left)
    right_proc->proc_left->proc_parent = positonProc;
  right_proc->proc_parent = positonProc->proc_parent;

  if (!positonProc->proc_parent)
    tree->root = right_proc;
  else if (positonProc->proc_parent->proc_left)
    positonProc->proc_parent->proc_left = right_proc;
  else 
    positonProc->proc_parent->proc_right = right_proc;

  right_proc->proc_left = positonProc;
  positonProc->proc_parent = right_proc;
}

// rotateRight
void rotateRight(struct RedBlack_Tree *tree, struct proc *positonProc) {
  struct proc *left_proc = positonProc->proc_left;
  
  positonProc->proc_left = left_proc->proc_right;
  if (left_proc->proc_right)
    left_proc->proc_right->proc_parent = positonProc;
  left_proc->proc_parent = positonProc->proc_parent;

  if (!positonProc->proc_parent)
    tree->root = left_proc;
  else if (positonProc->proc_parent->proc_right)
    positonProc->proc_parent->proc_right = left_proc;
  else 
    positonProc->proc_parent->proc_left = left_proc;

  left_proc->proc_right = positonProc;
  positonProc->proc_parent = left_proc;
}

// retrieveGrandproc_parentroc => retrive the grandparent of the passed process
struct proc *retrieveGrandproc_parentroc(struct proc* process) {
  if (process && process->proc_parent)
    return process->proc_parent->proc_parent;
  return 0;
}

// retrieveUncleProc => retrive the uncle of the passed process
struct proc *retrieveUncleProc(struct proc* process) {
  struct proc *grandParent = retrieveGrandproc_parentroc(process);
  if (grandParent) {
    if(process->proc_parent == grandParent->proc_left) {
      return grandParent->proc_right;
    } else {
      return grandParent->proc_left;
    }
  }
  return 0;
}

// setMinVruntime
struct proc *setMinVruntime(struct proc *traversingProcess) {
  if (!traversingProcess) {
    if (!traversingProcess->proc_left) 
      return setMinVruntime(traversingProcess->proc_left);
    else
      return traversingProcess;
  }
  return 0;
}

// insert_process
struct proc *insert_process (
  struct proc *traversing_process,
  struct proc *inserting_process) {
    inserting_process->proc_color = RED;
    if (!traversing_process)
      return traversing_process;

    if (traversing_process->virtual_runtime <= inserting_process->virtual_runtime) {
      inserting_process->proc_parent = traversing_process;
      traversing_process->proc_right = insert_process(traversing_process->proc_right, inserting_process);
    } else {
      inserting_process->proc_parent = traversing_process;
      traversing_process->proc_left = insert_process(traversing_process->proc_left, inserting_process);
    }

    return traversing_process;
}

/*
  handleInsertionCases(struct redblackTree*, struct proc*, int)
  parameters: tree - the pointer to the red black tree, process - the process in the red black tree, caseNumber - an integer value representing the case to be handled
  returns: none
  This function handles different cases that incorporate the properties of a red black tree. It uses the caseNumber to determine which case needs to be handled.
  cases:
  -1: if the inserted process is the root
  -2: if the parent of the inserted process is black
  -3: if both the parent and uncle processes are red, then repaint them black
  -4: if the parent is red and the uncle is black, but the inserted process is red and the inserted process is the right child of a parent that is left of the grandparent or vice versa
  -5: same as case four but the inserted process is the left child of a parent that is left of the grandparent or vice versa
*/
void
handleInsertionCases(struct RedBlack_Tree* tree, struct proc* process, int caseNumber){
	
  struct proc* uncleProcess;
  struct proc* grandparentProcess;
	
  switch(caseNumber){
  case 1:
	// If the inserted process is the root, color it black
	if(process->proc_parent == 0)
		process->proc_color = BLACK;
	else
		// If not, handle the next case
		handleInsertionCases(tree, process, 2);
	break;
	
  case 2:
	// If the parent of the inserted process is red, handle the next case
	if(process->proc_parent->proc_color == RED)
		handleInsertionCases(tree, process, 3);
	break;
	
  case 3:
	// Retrieve the uncle of the inserted process
	uncleProcess = retrieveUncleproc(process);
	
	// If the uncle exists and is red
	if(uncleProcess != 0 && uncleProcess->proc_color == RED){
		// Repaint the parent and uncle black
		process->proc_parent->proc_color = BLACK;
		uncleProcess->proc_color = BLACK;
		// Retrieve the grandparent of the inserted process
		grandparentProcess = retrieveGrandproc_parentroc(process);
		// Repaint the grandparent red
		grandparentProcess->proc_color = RED;
		// Handle the first case for the grandparent
		handleInsertionCases(tree, grandparentProcess, 1);
		grandparentProcess = 0;
	} else {
		// If the uncle is black or doesn't exist, handle the next case
		handleInsertionCases(tree, process,4);
	}
	
	uncleProcess = 0;
	break;
  
  case 4:
	// Retrieve the grandparent of the inserted process
	grandparentProcess = retrieveGrandproc_parentroc(process);
	
	// If the inserted process is the right child of a parent that is left of the grandparent
	if(process == process->proc_parent->proc_right && process->proc_parent == grandparentProcess->proc_left){
		// Rotate the tree to the left at the parent of the inserted process
		rotateLeft(tree, process->proc_parent);
		// Update the inserted process to its left child
		process = process->proc_left;
	} else if(process == process->proc_parent->proc_left && process->proc_parent == grandparentProcess->proc_right){
		// Rotate the tree to the right at the parent of the inserted process
		rotateRight(tree, process->proc_parent);
		// Update the inserted process to its right child
		process = process->proc_right;
	}
	// Handle the next case
	handleInsertionCases(tree, process, 5);
	grandparentProcess = 0;
	break;
	
  case 5:
    // Retrieve the grandparent of the inserted process
	grandparentProcess = retrieveGrandproc_parentroc(process);
	
	if(grandparentProcess != 0){
		// Repaint the grandparent red
		grandparentProcess->proc_color = RED;
		// Repaint the parent of the inserted process black
		process->proc_parent->proc_color = BLACK;
		// If the inserted process is the left child of a parent that is left of the grandparent
		if(process == process->proc_parent->proc_left && process->proc_parent == grandparentProcess->proc_left){
			// Rotate the tree to the right at the grandparent
			rotateRight(tree, grandparentProcess);
		} else if(process == process->proc_parent->proc_right && process->proc_parent == grandparentProcess->proc_right){
			// Rotate the tree to the left at the grandparent
			rotateLeft(tree, grandparentProcess);
		}
	}
	
	grandparentProcess = 0;
	break;
	
  default:
	break;
  }
  return;
}

// insert_process
void insertProcess(struct RedBlack_Tree* tree, struct proc* p){

  // Acquire the lock to ensure thread-safety
  acquire(&tree->lock);

  // Check if the tree is not full
  if(!fullTree(tree)){	
	// Insert the process into the tree
	tree->root = insertproc(tree->root, p);

	// If the tree was empty, set the parent of the root to null
	if(tree->count == 0)
		tree->root->proc_parent = 0;

	// Increment the count of processes in the tree
    	tree->count += 1;
	
	// Calculate the weight of the process based on its nice value
	p->proc_weight = computeProcessWeight(p->nice);

	// Add the weight of the process to the total weight of the tree
	tree->weight += p->proc_weight;
	
    	// Check for possible violations of Red-Black Tree properties and fix them
	insertionCases(tree, p, 1);
		
	// Find the process with the smallest vRuntime
	// If there was no insertion of a process that has a smaller minimum virtual runtime then the process that is being pointed by min_vruntime
	if(tree->min_vruntime == 0 || tree->min_vruntime->proc_left != 0)
		tree->min_vruntime = setMinimumVRuntimeproc(tree->root);
	 
  }	

  // Release the lock
  release(&tree->lock);
}

// retrieving_cases
/*
  handleRetrievalCases(struct RedBlack_Tree*, struct proc*, struct proc*, int)
  parameters: tree - The red black tree pointer to access and modify the root, parentProcess - the parent of the process, process - the pointer to the process with the smallest virtual Runtime, caseNumber - the case number
  returns: none
  This function checks for violations of the red black tree properties when we remove the process out of the tree and fixes them. 
  cases:
  -1: We remove the process that needs to be retrieved and ensure that either the process or the process's child is red, but not both of them.
  -2: If both the process we want to remove is black and it has a child that is black, then we perform recoloring and rotations to ensure red black tree property is met.
*/
void handleRetrievalCases(struct RedBlack_Tree* tree, struct proc* parentProcess, struct proc* process, int caseNumber){
  struct proc* grandparentProcess;
  struct proc* childProcess;
  struct proc* siblingProcess;
  
  switch(caseNumber){
	case 1:
		// Replace smallest virtual Runtime process with its right child 
		grandparentProcess = parentProcess;
		childProcess = process->proc_right;
		
		// If the process being removed is the root
		if(process == tree->root){
			tree->root = childProcess;
			if(childProcess != 0){
				childProcess->proc_parent = 0;
				childProcess->proc_color  = BLACK;
			}
		} else if(childProcess != 0 && !(process->proc_color  == childProcess->proc_color )){
			// Replace current process by its right child
			childProcess->proc_parent = grandparentProcess;
			grandparentProcess->proc_left = childProcess;
			childProcess->proc_color  = BLACK;		
		} else if(process->proc_color  == RED){		
			grandparentProcess->proc_left = childProcess;
		} else {	
			if(childProcess != 0)
				childProcess->proc_parent= grandparentProcess;
			
			grandparentProcess->proc_left = childProcess;
			handleRetrievalCases(tree, grandparentProcess, childProcess, 2);
		}
		
		process->proc_parent = 0;
		process->proc_left = 0;
		process->proc_right = 0;
		grandparentProcess = 0;
		childProcess = 0;
		break;
		
	case 2:
		// Check if process is not root,i.e parentProcess != 0, and process is black
		while(process != tree->root && (process == 0 || process->proc_color  == BLACK)){
			// Obtain sibling process
			if(process == parentProcess->proc_left){
				siblingProcess = parentProcess->proc_right;
				
				if(siblingProcess != 0 && siblingProcess->proc_color  == RED){
					siblingProcess->proc_color  = BLACK;
					parentProcess->proc_color  = RED;
					rotateLeft(tree, parentProcess);
					siblingProcess = parentProcess->proc_right;
				}
				if((siblingProcess->proc_left == 0 || siblingProcess->proc_left->proc_color  == BLACK) && (siblingProcess->proc_right == 0 || siblingProcess->proc_right->proc_color == BLACK)){
					siblingProcess->proc_color  = RED;
					// Change process pointer and parentProcess pointer
					process = parentProcess;
					parentProcess = parentProcess->proc_parent;
				} else {
					if(siblingProcess->proc_right == 0 || siblingProcess->proc_right->proc_color == BLACK){
						// Color left child
						if(siblingProcess->proc_left != 0){
							siblingProcess->proc_left->proc_color = BLACK;
						} 
						siblingProcess->proc_color  = RED;
						rotateRight(tree, siblingProcess);
						siblingProcess = parentProcess->proc_right;
					}
					
					siblingProcess->proc_color  = parentProcess->proc_color ;
					parentProcess->proc_color  = BLACK;
					siblingProcess->proc_right->proc_color  = BLACK;
					rotateLeft(tree, parentProcess);
					process = tree->root;
				}
			} 
		}
		if(process != 0)
			process->proc_color = BLACK;
		
		break;
	
	default:
		break;
  }
  return;
}

// retrieve_process
struct proc*
retrieveProcess(struct RedBlack_Tree* tree){
  struct proc* foundProcess;	//Process pointer utilized to hold the address of the process with smallest VRuntime 

  acquire(&tree->lock);
  if(!emptyTree(tree)){

	//If the number of processes are greater than the division between latency and minimum granularity
	//then recalculate the period for the processes
	//This condition is performed when the scheduler selects the next process to run
        //The formula can be found in CFS tuning article by Jacek Kobus and Refal Szklarski
	//In the CFS schduler tuning section:
	if(tree->count > (latency / min_gran )){
		tree->period = tree->count * min_gran ;
	} 

	//retrive the process with the smallest virtual runtime by removing it from the red black tree and returning it
	foundProcess = tree->min_vruntime;	

	//Determine if the process that is being chosen is runnable at the time of the selection, if it is not, then don't return it.
	if(foundProcess->state != RUNNABLE){
  		release(&tree->lock);
		return 0;
	}

	retrievingCases(tree, tree->min_vruntime->proc_parent, tree->min_vruntime, 1);
	tree->count -= 1;

	//Determine new process with the smallest virtual runtime
	tree->min_vruntime = setMinimumVRuntimeproc(tree->root);

	//Calculate retrieved process's time slice based on formula: period*(process's weight/ red black tree weight)
	//Where period is the length of the epoch
	//The formula can be found in CFS tuning article by Jacek Kobus and Refal Szklarski
	//In the scheduling section:
	foundProcess->max_exec_time = (tree->period * foundProcess->proc_weight / tree->weight);
	
	//Recalculate total weight of red-black tree
	tree->weight -= foundProcess->proc_weight;
  } else 
	foundProcess = 0;

  release(&tree->lock);
  return foundProcess;
}
/*
  evaluatePreemption(struct proc*, struct proc*)
  parameters: currentProcess - the currently running/selected process, minVruntimeProcess - the process with the smallest vruntime in the red black tree
  return: an integer value that dictates whether preemption should occur
  This function determines if the current process should be preempted.
  Preemption Cases:
  1- If the current running process virtual runtime is greater than the smallest virtual runtime
  2- If current running process current_runtime has exceeded the maximum execution time
  3- Allow the current running process to continue running until preemption should occur
*/
int evaluatePreemption(struct proc* currentProcess, struct proc* minVruntimeProcess){

  // Use an integer variable to compare current runtime with the minimum granularity
  int processRuntime = currentProcess->current_runtime;
  
  // Determine if the currently running process has exceeded its time slice.
  if((processRuntime >= currentProcess->max_exec_time) && (processRuntime >= min_gran )){
  	return 1;
  }

  // If the virtual runtime of the currently running process is greater than the smallest process, 
  // then context switching should occur
  if(minVruntimeProcess != 0 && minVruntimeProcess->state == RUNNABLE && currentProcess->virtual_runtime > minVruntimeProcess->virtual_runtime){
	
	// Allow preemption if the process has ran for at least the min_gran .
    // Due to the calls of checking for preemption, there needs to be made a distinction between when the preemption function
	// is called after a process has just be selected by the cfs scheduler and when a process has been currently running.
	if((processRuntime != 0) && (processRuntime >= min_gran )){
		return 1;
  	} else if(processRuntime == 0){
		return 1;
    }
  }

  // No preemption should occur
  return 0;
}

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  rbTree_init(tasks, "tasks");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
