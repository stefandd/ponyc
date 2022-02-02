use @pony_schedulers[U32]()
use @pony_active_schedulers[U32]()

primitive Scheduler
  """
  Provides functions that expose information about runtime schedulers.
  """

  fun schedulers(auth: RuntimeInfoAuth): U32 =>
    """
    Returns the maximum number of schedulers available to run actors.
    """
    @pony_schedulers()

  fun active_schedulers(auth: RuntimeInfoAuth): U32 =>
    """
    Returns the number of schedulers currently available to run actors.
    """
    @pony_active_schedulers()

  fun sleeping_schedulers(auth: RuntimeInfoAuth): U32 =>
    """
    Returns the number of schedulers that are currently sleeping and not
    available run actors. Schedulers are put to sleep if there isn't enough
    work to keep all of the possible schedulers busy.
    """
    @pony_schedulers() - @pony_active_schedulers()
