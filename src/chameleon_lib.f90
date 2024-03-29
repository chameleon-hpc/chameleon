module chameleon_lib
 use iso_c_binding
 implicit none

type, bind(c) :: map_entry 
  type(c_ptr) :: valptr
  integer(kind=c_size_t) :: size
  integer(kind=c_int)    :: type
end type map_entry

public:: map_entry

interface
 function chameleon_init() bind(c)
   implicit none
   integer :: chameleon_init
 end function chameleon_init

 function chameleon_thread_init() bind(c)
   implicit none
   integer :: chameleon_thread_init
 end function chameleon_thread_init

 function chameleon_determine_base_addresses(func) BIND(c) 
   use iso_c_binding
   implicit none
   type(c_ptr), intent(in) :: func
   integer:: chameleon_determine_base_addresses
   
 end function chameleon_determine_base_addresses

 type(c_ptr) function chameleon_create_annotation_container_fortran() BIND(c) 
    use iso_c_binding
    implicit none
  end function chameleon_create_annotation_container_fortran

  function chameleon_set_annotation_int_fortran(ann, value) BIND(c)
    use iso_c_binding
    implicit none
    type(c_ptr), value, intent(in) :: ann
    integer(kind=c_int), value, intent(in) :: value
    integer :: chameleon_set_annotation_int_fortran
  end function chameleon_set_annotation_int_fortran

  function chameleon_get_annotation_int_fortran(ann) BIND(c)
    use iso_c_binding
    implicit none
    type(c_ptr), value, intent(in) :: ann
    integer :: chameleon_get_annotation_int_fortran
  end function chameleon_get_annotation_int_fortran

 type(c_ptr) function chameleon_create_task_fortran(entry, nargs, args) bind(c, name="chameleon_create_task_fortran")
    use iso_c_binding
    implicit none
    type(c_funptr), value, intent(in) :: entry
    integer(kind=c_int), value, intent(in) :: nargs
    type(c_ptr), intent(in), value :: args
 end function chameleon_create_task_fortran

 function chameleon_add_task_fortran(task) bind(c, name="chameleon_add_task_fortran")
   use iso_c_binding
   implicit none
   type(c_ptr), value, intent(in) :: task
   integer:: chameleon_add_task_fortran
 end function chameleon_add_task_fortran

 function chameleon_distributed_taskwait(nowait) bind(c)
   use iso_c_binding
   implicit none
   integer(kind=c_int), intent(in) :: nowait
 
   integer :: chameleon_distributed_taskwait
 end function chameleon_distributed_taskwait

 function chameleon_finalize() bind(c)
   implicit none
   integer :: chameleon_finalize
 end function chameleon_finalize

 !void chameleon_set_callback_task_finish(cham_migratable_task_t *task, chameleon_external_callback_t func_ptr, void *func_param);

 function chameleon_set_callback_task_finish(task, func_ptr, func_param) bind(c, name="chameleon_set_callback_task_finish")
    use iso_c_binding
    implicit none
    type(c_ptr), value, intent(in) :: task
    type(c_funptr), value, intent(in) :: func_ptr
    type(c_ptr),value, intent(in) :: func_param
    integer :: chameleon_set_callback_task_finish
 end function chameleon_set_callback_task_finish

end interface

  contains
   type(c_ptr) function chameleon_create_task(entry, nargs, args)
     use iso_c_binding
     implicit none
     !procedure(),pointer, intent(in) :: entry
     type(c_funptr), intent(in) :: entry
     integer(kind=c_int) :: nargs
     type(map_entry),pointer,dimension(:), intent(in) :: args
     
     chameleon_create_task = chameleon_create_task_fortran(entry, nargs, c_loc(args(1)))
   end function chameleon_create_task


!  function chameleon_add_task_manual(entry, nargs, args)
!    use iso_c_binding
!    implicit none
!    procedure(),pointer, intent(in) :: entry
!    integer(kind=c_int) :: nargs
!    type(map_entry),dimension(:) :: args
!    integer(kind=c_int) :: chameleon_add_task_manual
   
!    chameleon_add_task_manual = chameleon_add_task_manual_fortran(c_funloc(entry), nargs, c_loc(args))
!  end function

end module


