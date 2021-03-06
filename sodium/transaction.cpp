/**
 * Copyright (c) 2012-2014, Stephen Blackheath and Anthony Jones
 * Released under a BSD3 licence.
 *
 * C++ implementation courtesy of International Telematics Ltd.
 */
#include <sodium/sodium.h>

using namespace std;
using namespace boost;

namespace sodium {

    namespace impl {
        
        void intrusive_ptr_add_ref(sodium::impl::listen_impl_func<sodium::impl::H_STREAM>* p)
        {
            spin_lock* l = spin_get_and_lock(p);
            p->counts.inc_stream();
            l->unlock();
        }

        void intrusive_ptr_release(sodium::impl::listen_impl_func<sodium::impl::H_STREAM>* p)
        {
            spin_lock* l = spin_get_and_lock(p);
            p->counts.dec_stream();
            p->update_and_unlock(l);
        }

        void intrusive_ptr_add_ref(sodium::impl::listen_impl_func<sodium::impl::H_STRONG>* p)
        {
            spin_lock* l = spin_get_and_lock(p);
            p->counts.inc_strong();
            l->unlock();
        }
        
        void intrusive_ptr_release(sodium::impl::listen_impl_func<sodium::impl::H_STRONG>* p)
        {
            spin_lock* l = spin_get_and_lock(p);
            p->counts.dec_strong();
            p->update_and_unlock(l);
        }

        void intrusive_ptr_add_ref(sodium::impl::listen_impl_func<sodium::impl::H_NODE>* p)
        {
            spin_lock* l = spin_get_and_lock(p);
            p->counts.inc_node();
            l->unlock();
        }

        void intrusive_ptr_release(sodium::impl::listen_impl_func<sodium::impl::H_NODE>* p)
        {
            spin_lock* l = spin_get_and_lock(p);
            p->counts.dec_node();
            p->update_and_unlock(l);
        }
        
        void holder::handle(const SODIUM_SHARED_PTR<node>& target, transaction_impl* trans, const light_ptr& value) const
        {
            if (handler)
                (*handler)(target, trans, value);
            else
                send(target, trans, value);
        }

    }

#if !defined(SODIUM_SINGLE_THREADED)
    mutex::mutex()
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&mx, &attr);
    }

    mutex::~mutex()
    {
        pthread_mutex_destroy(&mx);
    }
#endif

    partition::partition()
        : depth(0),
          processing_post(false)
    {
#if !defined(SODIUM_SINGLE_THREADED)
        pthread_key_create(&key, NULL);
#endif
    }

    partition::~partition()
    {
#if !defined(SODIUM_SINGLE_THREADED)
        pthread_key_delete(key);
#endif
    }

    void partition::post(const std::function<void()>& action)
    {
#if !defined(SODIUM_SINGLE_THREADED)
        mx.lock();
#endif
        postQ.push_back(action);
#if !defined(SODIUM_SINGLE_THREADED)
        mx.unlock();
#endif
    }

    void partition::process_post()
    {
#if !defined(SODIUM_SINGLE_THREADED)
        mx.lock();
        // Prevent it running on multiple threads at the same time, so posts
        // will be handled in order for the partition.
        if (!processing_post) {
            processing_post = true;
#endif
#if !defined(SODIUM_NO_EXCEPTIONS)
            try {
#endif
                while (postQ.begin() != postQ.end()) {
                    std::function<void()> action = *postQ.begin();
                    postQ.erase(postQ.begin());
#if !defined(SODIUM_SINGLE_THREADED)
                    mx.unlock();
#endif
                    action();
#if !defined(SODIUM_SINGLE_THREADED)
                    mx.lock();
#endif
                }
                processing_post = false;
#if !defined(SODIUM_NO_EXCEPTIONS)
            }
            catch (...) {
                processing_post = false;
                throw;
            }
#endif
#if !defined(SODIUM_SINGLE_THREADED)
        }
        mx.unlock();
#endif
    }

    partition* def_part::part()
    {
        static partition part;
        return &part;
    }

    namespace impl {

        node::node() : rank(0) {}
        node::node(rank_t rank_) : rank(rank_) {}
        node::~node()
        {
            for (SODIUM_FORWARD_LIST<node::target>::iterator it = targets.begin(); it != targets.end(); it++) {
                SODIUM_SHARED_PTR<node> targ = it->n;
                if (targ) {
                    boost::intrusive_ptr<listen_impl_func<H_STREAM> > li(
                        reinterpret_cast<listen_impl_func<H_STREAM>*>(listen_impl.get()));
                    targ->sources.remove(li);
                }
            }
        }

        bool node::link(void* holder, const SODIUM_SHARED_PTR<node>& targ)
        {
            bool changed;
            if (targ) {
                std::set<node*> visited;
                changed = targ->ensure_bigger_than(visited, rank);
                boost::intrusive_ptr<listen_impl_func<H_STREAM> > li(
                    reinterpret_cast<listen_impl_func<H_STREAM>*>(listen_impl.get()));
                targ->sources.push_front(li);
            }
            else
                changed = false;
            targets.push_front(target(holder, targ));
            return changed;
        }

        void node::unlink(void* holder)
        {
            SODIUM_FORWARD_LIST<node::target>::iterator this_it;
            for (SODIUM_FORWARD_LIST<node::target>::iterator last_it = targets.before_begin(); true; last_it = this_it) {
                this_it = last_it;
                ++this_it;
                if (this_it == targets.end())
                    break;
                if (this_it->h == holder) {
                    SODIUM_SHARED_PTR<node> targ = this_it->n;
                    targets.erase_after(last_it);
                    if (targ) {
                        boost::intrusive_ptr<listen_impl_func<H_STREAM> > li(
                            reinterpret_cast<listen_impl_func<H_STREAM>*>(listen_impl.get()));
                        targ->sources.remove(li);
                    }
                    break;
                }
            }
        }

        bool node::ensure_bigger_than(std::set<node*>& visited, rank_t limit)
        {
            if (rank > limit || visited.find(this) != visited.end())
                return false;
            else {
                visited.insert(this);
                rank = limit + 1;
                for (SODIUM_FORWARD_LIST<node::target>::iterator it = targets.begin(); it != targets.end(); ++it)
                    if (it->n)
                        it->n->ensure_bigger_than(visited, rank);
                return true;
            }
        }

        rank_t rankOf(const SODIUM_SHARED_PTR<node>& target)
        {
            if (target.get() != NULL)
                return target->rank;
            else
                return SODIUM_IMPL_RANK_T_MAX;
        }

        transaction_impl::transaction_impl(partition* part_)
            : part(part_),
              to_regen(false),
              inCallback(0)
        {
        }

        void transaction_impl::check_regen() {
            if (to_regen) {
                to_regen = false;
                prioritizedQ.clear();
                for (std::map<entryID, prioritized_entry>::iterator it = entries.begin(); it != entries.end(); ++it)
                    prioritizedQ.insert(pair<rank_t, entryID>(rankOf(it->second.target), it->first));
            }
        }

        transaction_impl::~transaction_impl()
        {
        }

        void transaction_impl::process_transactional()
        {
            while (true) {
                check_regen();
                std::multiset<pair<rank_t, entryID>>::iterator pit = prioritizedQ.begin();
                if (pit == prioritizedQ.end()) break;
                std::map<entryID, prioritized_entry>::iterator eit = entries.find(pit->second);
                assert(eit != entries.end());
                std::function<void(transaction_impl*)> action = eit->second.action;
                prioritizedQ.erase(pit);
                entries.erase(eit);
                action(this);
            }
            while (lastQ.begin() != lastQ.end()) {
                (*lastQ.begin())();
                lastQ.erase(lastQ.begin());
            }
        }

        void transaction_impl::prioritized(const SODIUM_SHARED_PTR<node>& target,
                                           const std::function<void(transaction_impl*)>& f)
        {
            entryID id = next_entry_id;
            next_entry_id = next_entry_id.succ();
            entries.insert(pair<entryID, prioritized_entry>(id, prioritized_entry(target, f)));
            prioritizedQ.insert(pair<rank_t, entryID>(rankOf(target), id));
        }

        void transaction_impl::last(const std::function<void()>& action)
        {
            lastQ.push_back(action);
        }

        transaction_::transaction_(partition* part)
            : impl_(policy::get_global()->current_transaction(part))
        {
            if (impl_ == NULL) {
                impl_ = new transaction_impl(part);
                policy::get_global()->initiate(impl_);
            }
            part->depth++;
        }
        
        transaction_::~transaction_()
        {
            close();
        }

        void transaction_::close()
        {
            impl::transaction_impl* impl__(this->impl_);
            if (impl__) {
                this->impl_ = NULL;
                partition* part = impl__->part;
                if (part->depth == 1) {
                    policy::get_global()->dispatch(
                        impl__,
                        [impl__] () {
                            impl__->process_transactional();
                            impl__->part->depth--;
                        },
                        [impl__] () {
                            partition* part_ = impl__->part;
                            delete impl__;
                            part_->process_post();
                        }
                    );
                }
                else
                    part->depth--;
            }
        }
    };  // end namespace impl

    static policy* global_policy = new simple_policy;

    /*static*/ policy* policy::get_global()
    {
        return global_policy;
    }

    /*static*/ void policy::set_global(policy* policy)
    {
        delete global_policy;
        global_policy = policy;
    }

    simple_policy::simple_policy()
    {
    }
    
    simple_policy::~simple_policy()
    {
    }

#if defined(SODIUM_SINGLE_THREADED)
	static impl::transaction_impl* global_transaction;
#endif

    impl::transaction_impl* simple_policy::current_transaction(partition* part)
    {
#if defined(SODIUM_SINGLE_THREADED)
    	return global_transaction;
#else
        return reinterpret_cast<impl::transaction_impl*>(pthread_getspecific(part->key));
#endif
    }

    void simple_policy::initiate(impl::transaction_impl* impl)
    {
#if defined(SODIUM_SINGLE_THREADED)
        global_transaction = impl;
#else
        impl->part->mx.lock();
        pthread_setspecific(impl->part->key, impl);
#endif
    }

    void simple_policy::dispatch(impl::transaction_impl* impl,
        const std::function<void()>& transactional,
        const std::function<void()>& post)
    {
        transactional();
#if defined(SODIUM_SINGLE_THREADED)
        global_transaction = NULL;
#else
        pthread_setspecific(impl->part->key, NULL);
        impl->part->mx.unlock();
#endif
        post();  // note: deletes 'impl'
    }

};  // end namespace sodium

