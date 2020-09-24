#include <atomic>
#include <iostream>
#include <string.h>
//#include <unistd.h>
#include <thread>
#include <stdlib.h>
//g++ TimeMain.cpp -std=c++11 -Os -march=native  -o output  -pthread -fexceptions

using std::cout;
using std::endl;

#define PAGE_SIZE 4096
#define CACHE_LINE_SIZE 64
#define NODE_SIZE 1620
//#define NODE_SIZE 1496
static inline void * my_align_malloc(size_t align, size_t size)
{
	void * ptr;
	// LINUX
	int ret = posix_memalign(&ptr, align, size);
	if (ret != 0) {

		abort();
	}
	
//	ptr = _aligned_malloc(size, align);

	return ptr;
}

static inline void  my_align_free(void * ptr)
{
	// LINUX
	free(ptr);
	//_aligned_free(ptr);
}


enum State { empty, set, handled}; // enum  is int we want somthing smaller as char
template <class T>
class MpScQueue {
	// LINUX
private:
//public:

	class Node {
	public:
		T data;
		std::atomic<char> is_set ; // if the node and its data are valid.  0- empty  , 1- set , 2 - handled
		//char cache_line_pad[56];
		Node() :data(), is_set(0)
		{
			
		}
		Node(T d) : data(d), is_set(0)
		{
			
		}

		Node(const Node &n)
		{
			data = n.data;
			is_set = n.is_set;
		}
	};
	typedef char cache_line_pad[64];
	typedef typename std::aligned_storage<sizeof(Node), std::alignment_of<cache_line_pad>::value>::type aligned_node;

	

	class bufferList {
	public:
		Node currbuffer[NODE_SIZE] alignas(CACHE_LINE_SIZE) ;
		
		std::atomic<bufferList*> next alignas(CACHE_LINE_SIZE);
		bufferList* prev  alignas(CACHE_LINE_SIZE);
		unsigned int head; // we have onr thread that takes out elelemts and it is the only one that moves the head 
		unsigned int positionInQueue; // start with 1 ( because i am using multiplication)

		bufferList()
			: next(NULL), prev(NULL), head(0), positionInQueue(0)
		{	
		}
		bufferList(unsigned int bufferSizez)
			:  next(NULL), prev(NULL), head(0), positionInQueue(1)
		{
			memset(currbuffer, 0, NODE_SIZE *sizeof(Node) );
		}
		bufferList(unsigned int bufferSizez , unsigned int positionInQueue , bufferList* prev)
			: next(NULL), prev(prev), head(0), positionInQueue(positionInQueue)

		{	
			memset(currbuffer, 0, NODE_SIZE * sizeof(Node));
		}
		

	};

	bufferList* headOfQueue; //the first array that contains data for the thread that takes elements from the queue (for the single consumer					
	std::atomic<bufferList*> tailOfQueue alignas(CACHE_LINE_SIZE);// the beginning of the last array which we insert elements into (for the producers)
	unsigned int bufferSize;
	std::atomic<uint_fast64_t> gTail alignas(CACHE_LINE_SIZE);// we need a global tail so the queue will be wait free - if not the queue is look free
	
	

public:
	MpScQueue(unsigned int size) :
		bufferSize(NODE_SIZE), tailOfQueue(NULL), gTail(0)
	{
		void* buffer = my_align_malloc(PAGE_SIZE, sizeof(bufferList));
		headOfQueue = new(buffer)bufferList(bufferSize);
		tailOfQueue = headOfQueue;
	}
	MpScQueue() :
		bufferSize(NODE_SIZE), tailOfQueue(NULL), gTail(0)
	{
		void* buffer = my_align_malloc(PAGE_SIZE, sizeof(bufferList));
		headOfQueue = new(buffer)bufferList(bufferSize);
		tailOfQueue = headOfQueue;
	}

	~MpScQueue()
	{

		while (headOfQueue->next.load(std::memory_order_acquire) != NULL)
		{
			bufferList* next = headOfQueue->next.load(std::memory_order_acquire);
			my_align_free( headOfQueue);
			headOfQueue = next;

		}
		my_align_free(headOfQueue);

	}
	// return false if the queue is empty 
	bool dequeue(T& data) // to take out an element - just one thread call this func 
	{
		while (true)
		{
			bufferList* tempTail = tailOfQueue.load(std::memory_order_seq_cst);
			unsigned int prevSize = bufferSize*(tempTail->positionInQueue - 1);
			if ((headOfQueue == tailOfQueue.load(std::memory_order_acquire)) && (headOfQueue->head == (gTail.load(std::memory_order_acquire) - prevSize) ))  // the queue is empty
			{
				//cout << "empty" << endl;
				return false;
			}
			else if (headOfQueue->head < bufferSize) // there is elements in the current array.
			{
				Node* n = &(headOfQueue->currbuffer[headOfQueue->head]);
				if (n->is_set.load(std::memory_order_acquire) == 2)
				{
					headOfQueue->head++;
          continue;
          cout << "handeld" << endl;
				}

				bufferList* tempHeadOfQueue = headOfQueue;
				unsigned int tempHead = tempHeadOfQueue->head;
				bool flag_moveToNewBuffer = false , flag_buffer_all_handeld=true;

				while (n->is_set.load(std::memory_order_acquire) == 0) // is not set yet - try to take out set elements that are next in line
				{
					//cout << "taking elements out after the first element" << endl;
				
					if (tempHead  < bufferSize) // there is elements in the current array.
					{
						Node* tn = &(tempHeadOfQueue->currbuffer[tempHead++]);
						if (tn->is_set.load(std::memory_order_acquire) == 1 && n->is_set.load(std::memory_order_acquire) == 0) // the data is valid and the element in the head is still in insert process 
						{

							//************* scan****************************
							bufferList* scanHeadOfQueue = headOfQueue;
							// we want to scan until one before tn 
							for (unsigned int scanHead = scanHeadOfQueue->head; ( scanHeadOfQueue != tempHeadOfQueue || scanHead < (tempHead-1) && n->is_set.load(std::memory_order_acquire) == 0); scanHead++)
							{
								if (scanHead >= bufferSize)   // we reach the end of the buffer -move to the next
								{
									scanHeadOfQueue= scanHeadOfQueue->next.load(std::memory_order_acquire);
									scanHead = scanHeadOfQueue->head;
									continue;
								}
								Node* scanN = &(scanHeadOfQueue->currbuffer[scanHead]);
								if (scanN->is_set.load(std::memory_order_acquire) == 1) // there is anther element that is set - the scan start again until him
								{
									// this is the new item to be evicted
									tempHead = scanHead;
									tempHeadOfQueue = scanHeadOfQueue;
									tn = scanN;
									//tn_is_set = tn->is_set.load(std::memory_order_acquire);
									
									scanHeadOfQueue = headOfQueue;
									scanHead = scanHeadOfQueue->head;
								}							
							}

							if (n->is_set.load(std::memory_order_acquire) == 1)
							{
								break;
							}
							//************* scan  end ****************************
							
							data = tn->data;
							tn->is_set.store(2, std::memory_order_release);//set taken
							if (flag_moveToNewBuffer && (tempHead-1) == tempHeadOfQueue->head) // if we moved to a new buffer ,we need to move forward the head so in the end we can delete the buffer
							{
								tempHeadOfQueue->head++;
							}
							//	cout << "realy taking" << endl;
							return true;

						}// the data is valid and the element in the head is still in insert process 
					
						 if(tn->is_set.load(std::memory_order_acquire) ==0)
						{ // tn->is_set.load(std::memory_order_acquire) == 0 empty 
							flag_buffer_all_handeld = false ;
						}
						
					}
					if (tempHead >= bufferSize)   // we reach the end of the buffer -move to the next
					{
						if (flag_buffer_all_handeld && flag_moveToNewBuffer) // we want to "fold" the queue - if there is a thread that got stuck - we want to keep only that buffer and delete the rest( upfront from  it) 
						{
							if (tempHeadOfQueue == tailOfQueue.load(std::memory_order_acquire)) return false; // there is no place to move 

							bufferList* next = tempHeadOfQueue->next.load(std::memory_order_acquire);
							bufferList* prev = tempHeadOfQueue->prev;
							if (next == NULL) return false; // if we do not have where to move 

							next->prev = prev;
							prev->next.store(next, std::memory_order_release);
							my_align_free(tempHeadOfQueue);
							//delete tempHeadOfQueue;
							tempHeadOfQueue = next;
							tempHead = tempHeadOfQueue->head;
							flag_buffer_all_handeld = true;
							flag_moveToNewBuffer = true;

						}
						else {
							bufferList* next = tempHeadOfQueue->next.load(std::memory_order_acquire);
							if (next == NULL) return false; // if we do not have where to move 
							tempHeadOfQueue = next;
							tempHead = tempHeadOfQueue->head;
							flag_moveToNewBuffer = true;
							flag_buffer_all_handeld = true;
							//cout << "going trough elements in queue " << endl;
						}
					}


				}//try to take out set elements that are next in line
				
				//---------------------------n->is_set.load() == State::empty-------------------------------------
				if (n->is_set.load(std::memory_order_acquire) == 1)//valid
				{
					headOfQueue->head++;
					data = n->data;
					//n->is_set.store(2, std::memory_order_seq_cst); //set taken
					return true;
				}
				
			}
			if (headOfQueue->head >= bufferSize) // move to the next array and delete the prev array 
			{
				if (headOfQueue == tailOfQueue.load(std::memory_order_acquire)) return false; // there is no place ro move 
																							  //delete[] headOfArray->currbuffer; // we finished reading the current array - we can delete it and move to the next one 
																							  //	headOfArray->currbuffer = NULL;
				bufferList* next = headOfQueue->next.load(std::memory_order_acquire);
				if (next == NULL) return false; // if we do not have where to move 
			//	cout << "^^^^^dequeue delete old  " << endl;
				my_align_free(headOfQueue);
				//delete headOfQueue;
				headOfQueue = next;
				//head = 0;
			}
		}
	}

	void enqueue(T const& data)// to put in an element
	{
		//bool did_fetch_add = false;
		bufferList* tempTail;
		unsigned int location = gTail.fetch_add(1, std::memory_order_seq_cst);
		bool go_back = false;
		while (true)
		{
		
			tempTail = tailOfQueue.load(std::memory_order_acquire);// points to the last buffer in queue 
			
			unsigned int prevSize = bufferSize*(tempTail->positionInQueue-1); // the amount of items in the queue without the current buffer

			while (location <prevSize) // the location is back in the queue - we need to go to back in the queue to the right buffer
			{
				go_back = true;
				tempTail = tempTail->prev;
				prevSize -= bufferSize;
			}

			unsigned int globalSize = bufferSize+ prevSize; // the amount of items in the queue 

			if (prevSize <= location && location < globalSize) // we are in the right buffer 
			{
				int index = location - prevSize;

				Node* n = &(tempTail->currbuffer[index]);
					n->data = data;
					n->is_set.store(1, std::memory_order_relaxed);// need this to signal the thread that the data is ready 
				//	n->is_set.store(1, std::memory_order_seq_cst);// need this to signal the thread that the data is ready 
					if (index == 1 && !go_back) // allocating a new buffer and adding it to the queue
					{
						void* buffer = my_align_malloc(PAGE_SIZE, sizeof(bufferList));
						bufferList* newArr =new(buffer)bufferList(bufferSize, tempTail->positionInQueue + 1, tempTail);
						bufferList* Nullptr = NULL;

						if (!(tempTail->next).compare_exchange_strong(Nullptr, newArr))
						{
							my_align_free(newArr);
						}
					}

					return;			
			}

			if (location >= globalSize) // the location we got is in the next buffer  
			{
				bufferList* next = (tempTail->next).load(std::memory_order_acquire);
				if (next == NULL) // we do not have yet a next buffer - so we can try to allocate a new one
				{

					void* buffer = my_align_malloc(PAGE_SIZE, sizeof(bufferList));
					bufferList* newArr = new(buffer)bufferList(bufferSize, tempTail->positionInQueue + 1, tempTail);
						bufferList* Nullptr = NULL;

						if ((tempTail->next).compare_exchange_strong(Nullptr, newArr))//	if (CAS(tailOfQueue->next, NULL, newArr)) 1 next-> new array
						{
							tailOfQueue.store(newArr, std::memory_order_release);
						}
						else
						{
							my_align_free(newArr);
							//delete  newArr; // if several threads try simultaneously and the cas fail one succeed - delete the rest 
						}
					
				}

				else 
				{
					tailOfQueue.compare_exchange_strong(tempTail, next);// if it is not null move to the next buffer;
				}
			}
				
			

		}//while
	}
	
};