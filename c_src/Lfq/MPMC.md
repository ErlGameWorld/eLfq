# 无锁队列的详细设计

2014 年 11 月 6 日发布 这篇文章非常详细地概述了我对支持多个并发生产者和消费者（MPMC 队列）的高效无锁队列的设计。我的这个设计的 C++11 实现可以在 GitHub
上找到。队列的更高级概述，包括基准测试结果，可以在我介绍我的实现的初始博客文章中找到。

一个关键的见解是元素从队列中出来的总顺序是无关紧要的，只要它们在给定线程上的顺序与它们在另一个线程上的顺序相匹配。这意味着队列可以安全地实现为一组独立的队列，每个生产者一个；编写一个单生产者、多消费者的无锁队列比编写一个多生产者、多消费者的队列容易得多，并且可以更有效地实现。通过让消费者根据需要从不同的
SPMC 队列中拉取数据，可以将 SPMC 队列推广为 MPMC 队列（这也可以通过一些巧妙的方式有效地完成）。在典型情况下，启发式用于加速出队，尽可能将消费者与生产者配对（这大大减少了系统中的整体争用）。

除了高级 SPMC
队列集设计之外，队列的另一个关键部分是核心排队算法本身，它是全新的（我自己设计的）并且与我听说过的任何其他算法不同。它使用原子计数器来跟踪有多少元素可用，并且一旦声明了一个或多个元素（通过增加相应的元素消耗计数器并检查该增量是否有效），原子索引可以安全地增加到获取要引用的元素的实际
ID。然后问题简化为将整数 ID 映射到单个元素，而不必担心其他线程引用相同的对象（每个 ID 仅分配给一个线程）。详情如下！

## 系统总览

该队列由一系列单生产者多消费者 (SPMC) 队列组成。每个生产者有一个 SPMC 队列；消费者使用启发式方法来确定从下一个消费这些队列中的哪一个。队列是无锁的（虽然不是完全无等待）。它被设计为健壮的、非常快的（尤其是在 x86
上），并且允许批量入队和出队，而额外的开销很少（与单个项目相比）。

每个生产者都需要一些线程本地数据，也可以选择使用线程本地数据来加速消费者。此线程本地数据可以与用户分配的令牌相关联，或者，为了简化接口，如果用户没有为生产者提供令牌，则使用无锁哈希表（以当前线程 ID
为键）查找线程本地生产者队列：为每个显式分配的生产者令牌创建一个 SPMC
队列，以及另一个隐式一个用于生成项目而不提供令牌的线程。由于令牌包含线程特定的数据，它们永远不应该在多个线程中同时使用（尽管将令牌的所有权转移到另一个线程是可以的；特别是，这允许在线程池任务中使用令牌，即使运行任务的线程中途更改）。

所有生产者队列都将自己链接到一个无锁链表中。当一个显式生产者不再有元素被添加到它时（即它的令牌被销毁），它被标记为与任何生产者无关，但它被保留在列表中并且它的内存不会被释放；下一个新生产者重用旧生产者的内存（无锁生产者列表是这样添加的）。隐式生产者永远不会被销毁（直到高级队列本身被销毁），因为无法知道给定的线程是否使用数据结构完成。请注意，最坏​​情况的出队速度取决于有多少生产者队列，即使它们都是空的。

显式和隐式生产者队列的生命周期存在根本区别：显式队列的生产生命周期与令牌的生命周期相关，而隐式队列的生命周期是无限的，并且存在于高级队列本身的持续时间内. 因此，使用了两种略有不同的 SPMC
算法，以最大限度地提高速度和内存使用率。一般来说，显式生产者队列设计得稍微快一点，占用更多内存，而隐式生产者队列设计得稍微慢一点，但将更多内存回收到高级队列的全局池中。为获得最佳速度，请始终使用显式令牌（除非您觉得它太不方便）。

任何分配的内存只有在高级队列被销毁时才会被释放（尽管有几种重用机制）。内存分配可以预先完成，如果没有足够的内存（而不是分配更多），操作就会失败。如果需要，使用者可以覆盖各种默认大小参数（以及队列使用的内存分配函数）。

## 完整的 API（伪代码）

# Allocates more memory if necessary

enqueue(item) : bool enqueue(prod_token, item) : bool enqueue_bulk(item_first, count) : bool enqueue_bulk(prod_token,
item_first, count) : bool

# Fails if not enough memory to enqueue

try_enqueue(item) : bool try_enqueue(prod_token, item) : bool try_enqueue_bulk(item_first, count) : bool
try_enqueue_bulk(prod_token, item_first, count) : bool

# Attempts to dequeue from the queue (never allocates)

try_dequeue(item&) : bool try_dequeue(cons_token, item&) : bool try_dequeue_bulk(item_first, max) : size_t
try_dequeue_bulk(cons_token, item_first, max) : size_t

# If you happen to know which producer you want to dequeue from

try_dequeue_from_producer(prod_token, item&) : bool try_dequeue_bulk_from_producer(prod_token, item_first, max) : size_t

# A not-necessarily-accurate count of the total number of elements

size_approx() : size_t

## 生产者队列 (SPMC) 设计

跨隐式和显式版本的共享设计 生产者队列由块组成（显式和隐式生产者队列都使用相同的块对象以实现更好的内存共享）。最初，它开始时没有块。每个块可以容纳固定数量的元素（所有块具有相同的容量，即 2
的幂）。此外，块包含一个标志，指示已填充的插槽是否已被完全消耗（由显式版本用于确定块何时为空），以及完全出列的元素数量的原子计数器（由隐式版本使用）版本以确定块何时为空）。

出于无锁操作的目的，生产者队列可以被认为是一个抽象的无限数组。甲尾索引指示用于生产者填充下一个可用时隙;
它还可以作为曾经入队的元素数量的计数（入队计数）。尾部索引由生产者单独写入，并且总是增加（除非它溢出并环绕，这对于我们的目的仍然被认为是“增加”）。由于只有一个线程正在更新所涉及的变量，因此生成一个项目是微不足道的。一个头指数表示接下来可以消耗什么元素。头索引由消费者原子地递增，可能是并发的。为了防止头索引到达/通过感知的尾索引，使用了一个额外的原子计数器：出队计数.
出队计数是乐观的，即当消费者推测性地相信有一些东西要出队时，它会被消费者增加。如果增加后出队计数的值小于入队计数（尾部），那么保证至少有一个元素出队（即使考虑并发性），并且增加头部索引是安全的，知道之后会小于尾部索引。另一方面，如果在递增后出队计数超过（或等于）尾部，则出队操作失败并且出队计数在逻辑上递减（以使其最终与入队计数保持一致）：这可以通过直接减少出队计数，而是（以增加并行性并保持所有涉及的变量单调增加）出队过量使用计数器会增加。为了得到出队计数的逻辑值，我们从出队计数变量中减去出队过量使用值。

在消费时，一旦确定了一个有效索引，它仍然需要映射到一个块和该块的偏移量；某种索引数据结构用于此目的（哪个取决于它是隐式队列还是显式队列）。最后，可以将元素移出，并更新某种状态，以便最终知道块何时完全用完。这些机制的完整描述在涵盖隐式和显式特定细节的各​​个部分中提供。

如前所述，尾部和头部索引/计数最终会溢出。这是预期的并已考虑在内。因此，索引/计数被认为存在于最大整数值大小的圆上（类似于 360 度的圆，其中 359 在 1
之前）。为了检查一个索引/计数是否a在另一个之前，例如b，（即逻辑小于），我们必须确定是否通过圆上的顺时针弧a更接近于b。使用以下循环小于算法（32 位版本）：a < b变成a - b > (1U << 31U). a <= b变成 a -
b - 1ULL > (1ULL << 31ULL).
请注意，循环减法“仅适用于”正常的无符号整数（假设为二进制补码）。注意确保尾索引不会超过头索引（这会破坏队列）。请注意，尽管如此，从技术上讲仍然存在竞争条件，其中消费者（或生产者，就此而言）看到的索引值是如此陈旧，以至于它几乎落后于其当前值整个圆圈的价值（或更多！），导致队列的内部状态被破坏。然而，在实践中，这不是问题，因为浏览
2^31 个值（对于 32 位索引类型）需要一段时间，届时其他内核将看到更新的内容。事实上，许多无锁算法都是基于相关的标记指针习语，其中前 16 位用于重复递增的标签，后 16
位用于指针值；这依赖于类似的假设，即一个核心不能在其他核心不知道的情况下增加标签超过 2^15 倍。然而，队列的默认索引类型是 64 位宽（即使 16 位似乎足够，即使在理论上也应该防止任何潜在的竞争）。

内存分配失败也得到了正确处理，永远不会破坏队列（它只是简单地报告为失败）。但是，假定元素本身在被队列操作时永远不会抛出异常。

## 块池

使用了两种不同的块池：首先，有预先分配的块的初始数组。一旦被消耗，这个池子永远是空的。这将其无等待实现简化为带有检查（以确保该索引在范围内）的单个获取和添加原子指令（以获取空闲块的下一个索引）。其次，有一个无锁（虽然不是无等待）全局空闲列表（“全局”意味着对高级队列是全局的）准备重用的已用块，作为无锁单独实现链表：头指针最初指向空（空）。要将一个块添加到空闲列表中，该块的下一个将指针设置为头指针，然后在头未更改的情况下使用比较和交换（CAS）更新头指针以指向块；如果是，则重复该过程（这是经典的无锁
CAS 循环设计模式）。要从空闲列表中删除块，使用了类似的算法：读取头块的下一个指针，然后将头设置为下一个指针（使用 CAS），条件是头在与此同时。为了避免 ABA 问题，每个块都有一个引用计数，它在执行 CAS
以删除一个块之前递增，然后递减；如果在引用计数大于 0
的情况下尝试将块重新添加到空闲列表，则设置一个标志，指示该块应该在空闲列表上，并且下一个完成持有最后一个引用的线程检查这个标志并在那个时候将块添加到列表中（这是有效的，因为我们不关心顺序）。我已经更详细地描述了这个无锁空闲列表的确切设计和实现在另一篇博文中。当生产者队列需要一个新块时，它首先检查初始块池，然后是全局空闲列表，只有当它找不到空闲块时才在堆上分配一个新块（或者失败，如果内存不允许分配）。

## 显式生产者队列

显式生产者队列实现为块的循环单链表。它在快速路径上是无等待的，但仅在需要从块池中获取块（或分配的新块）时才无锁；这仅在其内部块缓存全部已满（或没有，这是开始时的情况）时发生。

一旦一个块被添加到显式生产者队列的循环链表中，它就永远不会被删除。甲尾块指针由生产者认为指向哪些元素目前正在插入块;
当尾块已满时，检查下一个块以确定它是否为空。如果是，则更新尾块指针以指向该块；如果不是，则请求一个新块并将其插入紧跟在当前尾块之后的链表中，然后更新该块以指向该新块。

当一个元素完成从块中出队时，每个元素的标志被设置以指示槽已满。（实际上，所有标志都从设置开始，并且仅在槽变空时才关闭。）生产者通过检查所有这些标志来检查块是否为空。如果块大小很小，这已经足够快了；否则，对于较大的块，而不是标志系统，每次消费者完成一个元素时，块级原子计数都会增加。当这个计数等于块的大小，或者所有标志都关闭时，块是空的，可以安全地重用。

为了在恒定时间内索引块（即快速找到元素所在的块，从出队算法中给定元素的全局索引），使用循环缓冲区（连续数组）。该索引由生产者维护；消费者从中读取但从不写入。数组的前面是最近写入的块（尾块）。在后方数组的最后一个块中可能有元素。将此索引（从高级角度）视为已使用区块历史的一条长带会很有帮助。每当生产者在另一个块上启动时（可能是新分配的，也可能是从其循环的块列表中重新使用），前端就会增加。每当循环列表中已经存在的块被重新使用时，后部就会增加（因为块只在它们为空时才被重新使用，在这种情况下增加后部总是安全的）。不是显式存储后部，而是保留所用插槽数的计数（这避免了循环缓冲区中备用元素的需要，并简化了实现）。如果索引中没有足够的空间来添加新项目，分配一个新的索引数组，它是前一个数组大小的两倍（显然，这仅在允许内存分配的情况下才允许——如果不允许，则整个入队操作正常失败）。由于消费者仍然可以使用旧索引，它不会被释放，而是简单地链接到新索引（这形成了一个索引块链，当高级队列被破坏时可以正确释放）。当生产者队列的入队计数增加时，它释放对索引的所有写入；当消费者执行获取（它已经需要出队算法）时，从那时起消费者看到的任何索引都将包含对消费者感兴趣的块的引用。由于块的大小都相同，并且2的幂，我们可以使用移位和掩码来确定我们的目标块与索引中的任何其他块（以及目标块中的偏移量）的偏移量，前提是我们知道索引中给定块的基本索引。因此，索引不仅包含块指针，还包含每个块的相应基索引。在索引中被选为参考点（以计算偏移量）的块在使用时不得被生产者覆盖——使用索引的（感知）前端作为参考点保证了这一点，因为（知道块索引至少与我们正在查找的出队索引之前的入队计数一样最新）索引的前面必须位于或在目标块之前，并且目标块在索引中永远不会被覆盖，直到它（以及之前的所有块）为空，并且在出队操作本身完成之前它不能为空。索引大小是
2 的幂，它允许更快地包装前/后变量。

显式生产者队列要求在入队时传递用户分配的“生产者令牌”。该令牌仅包含指向生产者队列对象的指针。创建token时，会创建相应的生产者队列；当令牌被销毁时，生产者队列可能仍然包含未使用的元素，因此队列本身比令牌更长寿。事实上，一旦分配，生产者队列永远不会被销毁（直到高级队列被销毁），但它会在下一次创建生产者令牌时重新使用（而不是为新的生产者队列分配堆）。

## 隐式生产者队列

隐式生产者队列被实现为一组未链接的块。它是无锁的，但不是无等待的，因为主空闲块空闲列表的实现是无锁的，并且块不断地从该池中获取并插入回该池（调整块索引的大小也不是常数时间，并且需要内存分配）。实际的入队和出队操作仍然在单个块内等待空闲。

甲当前块指针被保持;
这是当前正在排队的块。当一个块填满时，会申请一个新的，而旧的（从生产者的角度来看）会被遗忘。在将元素添加到块之前，块被插入到块索引中（这允许消费者找到生产者已经忘记的块）。当块中的最后一个元素被消耗完时，该块从块索引中逻辑删除。

隐式生产者队列永远不会被重用——一旦创建，它就会在高级队列的整个生命周期中存在。因此，为了减少内存消耗，它不会占用它曾经使用过的所有块（如显式生产者），而是将用过的块返回到全局空闲列表中。为此，只要消费者完成项目的出队，每个块中的原子出队计数就会增加；当计数器达到块的大小时，看到这一点的消费者知道它刚刚出列了最后一项，并将该块放入全局空闲列表中。

隐式生产者队列使用循环缓冲区来实现其块索引，这允许在给定基本索引的情况下恒定时间搜索块。每个索引条目由表示块的基本索引的键值对和指向相应块本身的指针组成。由于块总是按顺序插入，索引中每个块的基索引保证在相邻条目之间增加恰好一个块大小的值。这意味着通过查看最后插入的基索引、计算所需基索引的偏移量并在该偏移量处查找索引条目，可以轻松找到索引中已知的任何块。

当一个块被花费时，它会从索引中移除（为以后的块插入腾出空间）；因为另一个消费者仍然可以使用索引中的那个条目（来计算一个偏移量），索引条目没有被完全删除，而是块指针被设置为空，向生产者指示插槽可以重新用过的;
对于仍在使用它来计算偏移量的任何消费者，块基数保持不变。由于生产者仅在所有前面的插槽也空闲时才重新使用插槽，并且当消费者在索引中查找块时，索引中必须至少有一个非空闲插槽（对应于它正在查找的块）
，并且消费者用于查找块的块索引条目至少与该块的索引条目最近排入队列，

当消费者希望将一个项目入队并且块索引中没有空间时，它（如果允许）分配另一个块索引（链接到旧的，以便在队列被破坏时它的内存最终可以被释放），这成为从此主索引。新索引是旧索引的副本，除了两倍大；复制所有索引条目允许消费者只需要查看一个索引即可找到他们要查找的块（块内出队的时间恒定）。因为在构造新索引时，消费者可能正在将索引条目标记为空闲（通过将块指针设置为空），所以索引条目本身不会被复制，而是指向它们的指针。这确保消费者对旧索引的任何更改也会正确影响当前索引。

## 隐式生产者队列的哈希

无锁哈希表用于将线程 ID 映射到隐式生产者；当没有为各种入队方法提供明确的生产者令牌时使用。它基于Jeff Preshing 的无锁哈希算法，有一些调整：键的大小与平台相关的数字线程 ID
类型相同；值是指针；当散列变得太小（元素的数量用原子计数器跟踪）时，会分配一个新的并将其链接到旧的，并且旧的元素在读取时会延迟传输。由于元素数量的原子计数可用，并且元素永远不会被删除，因此希望在哈希表中插入一个太小的元素的线程必须尝试调整大小或等待另一个线程完成调整大小（调整大小受保护带有锁以防止虚假分配）。为了在争用情况下加速调整大小（即最小化线程等待另一个线程完成分配的自旋等待量），

## 生产者链表

如前所述，维护所有生产者的单链接 (LIFO) 列表。这个列表是使用尾指针实现的，每个生产者的下一个（实际上，“上一个”）指针都是侵入性的。尾部最初指向空；当一个新的生产者被创建时，它首先读取尾部，然后使用设置它紧挨着尾部，然后使用 CAS
操作将尾部（如果它没有改变）设置为新的生产者（如果它没有改变）将自己添加到列表中（根据需要循环）。生产者永远不会从列表中删除，但可以标记为不活动。

当消费者想要出列一个项目时，它只是遍历生产者列表，寻找其中包含一个项目的 SPMC 队列（由于生产者的数量是无限的，这在一定程度上是使高级队列只是无锁的部分原因）免等待）。

## 出队启发式

消费者可以将令牌传递给各种出队方法。此令牌的目的是加快选择适当的内部生产者队列以尝试从中出队。使用一个简单的方案，其中每个显式消费者都被分配一个自动递增的偏移量，表示它应该出队的生产者队列的索引。通过这种方式，消费者在生产者之间尽可能公平地分配；然而，并非所有生产者都有相同数量的可用元素，一些消费者可能比其他消费者消费得更快；为了解决这个问题，从同一个内部生产者队列连续消费
256
个项目的第一个消费者增加了一个全局偏移量，导致所有消费者在他们的下一个出队操作上轮换并从下一个生产者开始消费。（请注意，这意味着旋转速率由最快的消费者决定。）如果消费者指定的队列中没有可用的元素，它会移动到下一个有可用元素的队列。这种简单的启发式方法是有效的，并且能够以近乎完美的扩展将消费者与生产者配对，从而实现令人印象深刻的出队加速。

## 关于线性化的说明

如果数据结构的所有操作似乎都以某种顺序（线性）顺序执行，即使在并发下，它也是可线性化的（本文有一个很好的定义）。虽然这是一个有用的特性，因为它使并发算法明显正确且更容易推理，但它是一个非常强大的特性一致性模型。我在这里介绍的队列是不可线性化的，因为这样做会导致性能更差；但是，我相信它仍然非常有用。我的队列具有以下一致性模型：任何给定线程上的入队操作（显然）在该线程上可线性化，但不是其他线程（这应该无关紧要，因为即使使用完全可线性化的队列，元素的最终顺序也是不确定的，因为它取决于在线程之间的比赛中）。请注意，即使入队操作不能跨线程线性化，它们仍然是原子的——只有完全入队的元素才能出队。如果所有生产者队列在检查时都显示为空，则允许出队操作失败.
这意味着出队操作也是非线性的，因为在出​​队操作失败期间，整个队列不一定在任何一点都为空。（即使单个生产者队列的空检查在技术上也是非线性的，因为入队操作可能完成但内存效应尚未传播到出队线程 - 同样，这无论如何都无关紧要，因为它取决于非-
无论哪种方式，确定性竞争条件。）

这种非线性在实际中意味着如果还有其他生产者在排队（无论其他线程是否正在出队），则出队操作可能会在队列完全清空之前失败。请注意，即使使用完全可线性化的队列，这种竞争条件仍然存在。如果队列已经稳定（即所有入队操作都已完成，并且它们的内存效应对任何潜在的消费者线程都可见），那么只要队列不为空，出队操作就永远不会失败。类似地，如果给定的一组元素对所有出队线程都是可见的，则这些线程上的出队操作将永远不会失败，直到至少该组元素被消耗为止（但即使队列不是完全空的，之后也可能会失败）。

## 结论

所以你有它！关于我的通用无锁队列设计，您想知道的比您想的要多。我已经使用 C++11 实现了这个设计，但我确信它可以移植到其他语言。如果有人确实用另一种语言实现了这个设计，我很乐意听到它！